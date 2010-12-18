/*******************************************************************************
 * arch/arm/src/lpc17xx/lpc17_usbhost.c
 *
 *   Copyright (C) 2010 Gregory Nutt. All rights reserved.
 *   Authors: Rafael Noronha <rafael@pdsolucoes.com.br>
 *            Gregory Nutt <spudmonkey@racsa.co.cr>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 * 3. Neither the name NuttX nor the names of its contributors may be
 *    used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 * OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 * ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 *******************************************************************************/

/*******************************************************************************
 * Included Files
 *******************************************************************************/

#include <nuttx/config.h>

#include <sys/types.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <debug.h>

#include <nuttx/arch.h>
#include <nuttx/usb/usb.h>
#include <nuttx/usb/ohci.h>
#include <nuttx/usb/usbhost.h>
#include <nuttx/usb/usbhost_trace.h>

#include <arch/irq.h>
#include <arch/board/board.h>

#include "chip.h"
#include "up_arch.h"
#include "up_internal.h"

#include "lpc17_internal.h"
#include "lpc17_usb.h"
#include "lpc17_syscon.h"

/*******************************************************************************
 * Definitions
 *******************************************************************************/

/* Frame Interval */

#define  FI                     (12000-1) /* 12000 bits per frame (-1) */
#define  DEFAULT_FMINTERVAL     ((((6 * (FI - 210)) / 7) << 16) | FI)

/* CLKCTRL enable bits */

#define LPC17_CLKCTRL_ENABLES (USBOTG_CLK_HOSTCLK|USBOTG_CLK_DEVCLK|\
                               USBOTG_CLK_I2CCLK|USBOTG_CLK_OTGCLK|\
                               USBOTG_CLK_AHBCLK)

/* Dump GPIO registers */

#if defined(CONFIG_LPC17_USBHOST_REGDEBUG) && defined(CONFIG_DEBUG_GPIO)
#  define usbhost_dumpgpio() \
   do { \
     lpc17_dumpgpio(GPIO_USB_DP, "D+ P0.29; D- P0.30"); \
     lpc17_dumpgpio(GPIO_USB_UPLED, "LED P1:18; PPWR P1:19 PWRD P1:22 PVRCR P1:27"); \
   } while (0);
#else
#  define usbhost_dumpgpio()
#endif

/* USB Host Memory *************************************************************/

#warning "Needs to be removed from the heap in lpc17_allocateheap.c"
#define USBHOST_SRAM_BASE LPC17_SRAM_BANK1

/* Debug ***********************************************************************/

/* Trace error codes */
#warning "To be provided"

/* Trace interrupt codes */
#warning "To be provided"

/*******************************************************************************
 * Private Types
 *******************************************************************************/

/* This structure retains the state of the USB host controller */

struct lpc17_usbhost_s
{
  /* Common device fields.  This must be the first thing defined in the
   * structure so that it is possible to simply cast from struct usbhost_s
   * to structlpc17_usbhost_s.
   */

  struct usbhost_driver_s usbhost;

  /* The bound device class driver */

  struct usbhost_class_s *class;
};

/* HostController EndPoint Descriptor */

struct usbhost_hced_s
{
  volatile  uint32_t  control;         /* Endpoint descriptor control */
  volatile  uint32_t  tailtd;          /* Physical address of tail in Transfer descriptor list */
  volatile  uint32_t  headtd;          /* Physical address of head in Transfer descriptor list */
  volatile  uint32_t  hext;            /* Physical address of next Endpoint descriptor */
};

/* HostController Transfer Descriptor */

struct usbhost_hctd_s
{                       
  volatile  uint32_t  control;         /* Transfer descriptor control */
  volatile  uint32_t  currbufptr;      /* Physical address of current buffer pointer */
  volatile  uint32_t  bext;            /* Physical pointer to next Transfer Descriptor */
  volatile  uint32_t  bufend;          /* Physical address of end of buffer */
};

/* Host Controller Communication Area */

struct usbhost_hcca_s
{
  volatile  uint32_t  inttable[32];     /* Interrupt table */
  volatile  uint32_t  framenumber;      /* Frame number */
  volatile  uint32_t  donehead;         /* Done head */
  volatile  uint8_t   reserved[116];    /* Reserved for future use */
  volatile  uint8_t   unknown[4];       /* Unused */
};

/* Helper definitions */

#define Hcca        ((volatile struct usbhost_hcca_s *)(USBHOST_SRAM_BASE + 0x000))
#define TDHead      ((volatile struct usbhost_hctd_s *)(USBHOST_SRAM_BASE + 0x100))
#define TDTail      ((volatile struct usbhost_hctd_s *)(USBHOST_SRAM_BASE + 0x110))
#define EDCtrl      ((volatile struct usbhost_hced_s *)(USBHOST_SRAM_BASE + 0x120))
#define EDBulkIn    ((volatile struct usbhost_hced_s *)(USBHOST_SRAM_BASE + 0x130))
#define EDBulkOut   ((volatile struct usbhost_hced_s *)(USBHOST_SRAM_BASE + 0x140))
#define TDBuffer    ((volatile uint8_t *)(USBHOST_SRAM_BASE + 0x150))

/*******************************************************************************
 * Private Function Prototypes
 *******************************************************************************/

/* Register operations ********************************************************/

#ifdef CONFIG_LPC17_USBHOST_REGDEBUG
static void lpc17_printreg(uint32_t addr, uint32_t val, bool iswrite);
static void lpc17_checkreg(uint32_t addr, uint32_t val, bool iswrite);
static uint32_t lpc17_getreg(uint32_t addr);
static void lpc17_putreg(uint32_t val, uint32_t addr);
#else
# define lpc17_getreg(addr)     getreg32(addr)
# define lpc17_putreg(val,addr) putreg32(val,addr)
#endif

/* Interrupt handling **********************************************************/

static int lpc17_usbinterrupt(int irq, FAR void *context);

/* USB host controller operations **********************************************/

static int usbhost_enumerate(FAR struct usbhost_driver_s *drvr);
static int usbhost_alloc(FAR struct usbhost_driver_s *drvr,
                         FAR uint8_t **buffer, FAR size_t *maxlen);
static int usbhost_free(FAR struct usbhost_driver_s *drvr, FAR uint8_t *buffer);
static int usbhost_control(FAR struct usbhost_driver_s *drvr,
                           const struct usb_ctrlreq_s *req, FAR uint8_t *buffer);
static int usbhost_transfer(FAR struct usbhost_driver_s *drvr,
                            FAR struct usbhost_epdesc_s *ed,
                            FAR uint8_t *buffer, size_t buflen);
static void usbhost_disconnect(FAR struct usbhost_driver_s *drvr);
  
/* Initializaion ***************************************************************/

static void usbhost_tdinit(volatile struct usbhost_hctd_s *td);
static void usbhost_edinit(volatile struct usbhost_hced_s *ed);
static void usbhost_hccainit(volatile struct usbhost_hcca_s *hcca);

/*******************************************************************************
 * Private Data
 *******************************************************************************/

/* In this driver implementation, support is provided for only a single a single
 * USB device.  All status information can be simply retained in a single global
 * instance.
 */

static struct lpc17_usbhost_s g_usbhost =
{
  .usbhost        =
    {
      .enumerate  = usbhost_enumerate,
      .alloc      = usbhost_alloc,
      .free       = usbhost_free,
      .control    = usbhost_control,
      .transfer   = usbhost_transfer,
      .disconnect = usbhost_disconnect,
    },
  .class          = NULL,
};

/*******************************************************************************
 * Public Data
 *******************************************************************************/

/*******************************************************************************
 * Private Functions
 *******************************************************************************/

/*******************************************************************************
 * Name: lpc17_printreg
 *
 * Description:
 *   Print the contents of an LPC17xx register operation
 *
 *******************************************************************************/

#ifdef CONFIG_LPC17_USBHOST_REGDEBUG
static void lpc17_printreg(uint32_t addr, uint32_t val, bool iswrite)
{
  lldbg("%08x%s%08x\n", addr, iswrite ? "<-" : "->", val);
}
#endif

/*******************************************************************************
 * Name: lpc17_checkreg
 *
 * Description:
 *   Get the contents of an LPC17xx register
 *
 *******************************************************************************/

#ifdef CONFIG_LPC17_USBHOST_REGDEBUG
static void lpc17_checkreg(uint32_t addr, uint32_t val, bool iswrite)
{
  static uint32_t prevaddr = 0;
  static uint32_t preval = 0;
  static uint32_t count = 0;
  static bool     prevwrite = false;

  /* Is this the same value that we read from/wrote to the same register last time?
   * Are we polling the register?  If so, suppress the output.
   */

  if (addr == prevaddr && val == preval && prevwrite == iswrite)
    {
      /* Yes.. Just increment the count */

      count++;
    }
  else
    {
      /* No this is a new address or value or operation. Were there any
       * duplicate accesses before this one?
       */

      if (count > 0)
        {
          /* Yes.. Just one? */

          if (count == 1)
            {
              /* Yes.. Just one */

              lpc17_printreg(prevaddr, preval, prevwrite);
            }
          else
            {
              /* No.. More than one. */

              lldbg("[repeats %d more times]\n", count);
            }
        }

      /* Save the new address, value, count, and operation for next time */

      prevaddr  = addr;
      preval    = val;
      count     = 0;
      prevwrite = iswrite;

      /* Show the new regisgter access */

      lpc17_printreg(addr, val, iswrite);
    }
}
#endif

/*******************************************************************************
 * Name: lpc17_getreg
 *
 * Description:
 *   Get the contents of an LPC17xx register
 *
 *******************************************************************************/

#ifdef CONFIG_LPC17_USBHOST_REGDEBUG
static uint32_t lpc17_getreg(uint32_t addr)
{
  /* Read the value from the register */

  uint32_t val = getreg32(addr);

  /* Check if we need to print this value */

  lpc17_checkreg(addr, val, false);
  return val;
}
#endif

/*******************************************************************************
 * Name: lpc17_putreg
 *
 * Description:
 *   Set the contents of an LPC17xx register to a value
 *
 *******************************************************************************/

#ifdef CONFIG_LPC17_USBHOST_REGDEBUG
static void lpc17_putreg(uint32_t val, uint32_t addr)
{
  /* Check if we need to print this value */

  lpc17_checkreg(addr, val, true);

  /* Write the value */

  putreg32(val, addr);
}
#endif

/*******************************************************************************
 * Name: lpc17_usbinterrupt
 *
 * Description:
 *   USB interrupt handler
 *
 *******************************************************************************/

static int lpc17_usbinterrupt(int irq, FAR void *context)
{
  struct lpc17_usbhost_s *priv = &g_usbhost;

  /* Read the device interrupt status register */

  usbtrace(TRACE_INTENTRY(LPC17_TRACEINTID_USB), 0xbeef);
#warning "Not implemented"
  usbtrace(TRACE_INTEXIT(LPC17_TRACEINTID_USB), 0);
  return OK;
}

/*******************************************************************************
 * USB Host Controller Operations
 *******************************************************************************/

/************************************************************************************
 * Name: usbhost_enumerate
 *
 * Description:
 *   Enumerate the connected device.  This function will enqueue the
 *   enumeration process.  As part of this enumeration process, the driver
 *   will (1) get the device's configuration descriptor, (2) extract the class
 *   ID info from the configuration descriptor, (3) call usbhost_findclass()
 *   to find the class that supports this device, (4) call the create()
 *   method on the struct usbhost_registry_s interface to get a class
 *   instance, and finally (5) call the configdesc() method of the struct
 *   usbhost_class_s interface.  After that, the class is in charge of the
 *   sequence of operations.
 *
 * Input Parameters:
 *   drvr - The USB host driver instance obtained as a parameter from the call to
 *      the class create() method.
 *
 * Returned Values:
 *   On success, zero (OK) is returned. On a failure, a negated errno value is
 *   returned indicating the nature of the failure
 *
 * Assumptions:
 *   This function will *not* be called from an interrupt handler.
 *
 ************************************************************************************/

static int usbhost_enumerate(FAR struct usbhost_driver_s *drvr)
{
# warning "Not Implemented"
  return -ENOSYS;
}

/************************************************************************************
 * Name: usbhost_alloc
 *
 * Description:
 *   Some hardware supports special memory in which transfer descriptors can
 *   be accessed more efficiently.  This method provides a mechanism to allocate
 *   the transfer descriptor memory.  If the underlying hardware does not support
 *   such "special" memory, this functions may simply map to malloc.
 *
 * Input Parameters:
 *   drvr - The USB host driver instance obtained as a parameter from the call to
 *      the class create() method.
 *   buffer - The address of a memory location provided by the caller in which to
 *     return the allocated buffer memory address.
 *   maxlen - The address of a memory location provided by the caller in which to
 *     return the maximum size of the allocated buffer memory.
 *
 * Returned Values:
 *   On success, zero (OK) is returned. On a failure, a negated errno value is
 *   returned indicating the nature of the failure
 *
 * Assumptions:
 *   This function will *not* be called from an interrupt handler.
 *
 ************************************************************************************/

 static int usbhost_alloc(FAR struct usbhost_driver_s *drvr,
                         FAR uint8_t **buffer, FAR size_t *maxlen)
{
# warning "Not Implemented"
  return -ENOSYS;
}

/************************************************************************************
 * Name: usbhost_free
 *
 * Description:
 *   Some hardware supports special memory in which transfer descriptors can
 *   be accessed more efficiently.  This method provides a mechanism to free that
 *   transfer descriptor memory.  If the underlying hardware does not support
 *   such "special" memory, this functions may simply map to free().
 *
 * Input Parameters:
 *   drvr - The USB host driver instance obtained as a parameter from the call to
 *      the class create() method.
 *   buffer - The address of the allocated buffer memory to be freed.
 *
 * Returned Values:
 *   On success, zero (OK) is returned. On a failure, a negated errno value is
 *   returned indicating the nature of the failure
 *
 * Assumptions:
 *   This function will *not* be called from an interrupt handler.
 *
 ************************************************************************************/

static int usbhost_free(FAR struct usbhost_driver_s *drvr, FAR uint8_t *buffer)
{
# warning "Not Implemented"
  return -ENOSYS;
}

/************************************************************************************
 * Name: usbhost_control
 *
 * Description:
 *   Enqueue a request on the control endpoint.  This method will enqueue
 *   the request and return immediately.  The transfer will be performed
 *   asynchronously.  When the transfer completes, the USB host driver will
 *   call the complete() method of the struct usbhost_class_s interface.
 *   Only one transfer may be queued; Neither this method nor the transfer()
 *   method can be called again until the class complete() method has been called.
 *
 * Input Parameters:
 *   drvr - The USB host driver instance obtained as a parameter from the call to
 *      the class create() method.
 *   req - Describes the request to be sent.  This data will be copied from the
 *      user provided memory.  Therefore, the req buffer may be declared on the
 *      stack.
 *   buffer - A buffer used for sending the request and for returning any
 *     responses.  This buffer must be large enough to hold the length value
 *     in the request description. buffer must have been allocated using DRVR_ALLOC
 *
 * Returned Values:
 *   On success, zero (OK) is returned. On a failure, a negated errno value is
 *   returned indicating the nature of the failure
 *
 * Assumptions:
 *   This function will *not* be called from an interrupt handler.
 *
 ************************************************************************************/

static int usbhost_control(FAR struct usbhost_driver_s *drvr,
                           const struct usb_ctrlreq_s *req, FAR uint8_t *buffer)
{
# warning "Not Implemented"
  return -ENOSYS;
}

/************************************************************************************
 * Name: usbhost_transfer
 *
 * Description:
 *   Enqueue a request to handle a transfer descriptor.  This method will
 *   enqueue the transfer request and return immediately.  The transfer will
 *   be performed asynchronously.  When the transfer completes, the USB host
 *   driver will call the complete() method of the struct usbhost_class_s
 *   interface.  Only one transfer may be queued; Neither this method nor the
 *   control method can be called again until the class complete() method has
 *   been called.
 *
 * Input Parameters:
 *   drvr - The USB host driver instance obtained as a parameter from the call to
 *      the class create() method.
 *   ed - The IN or OUT endpoint descriptor for the device endpoint on which to
 *      perform the transfer.
 *   buffer - A buffer containing the data to be sent (OUT endpoint) or received
 *     (IN endpoint).  buffer must have been allocated using DRVR_ALLOC
 *   buflen - The length of the data to be sent or received.
 *
 * Returned Values:
 *   On success, zero (OK) is returned. On a failure, a negated errno value is
 *   returned indicating the nature of the failure
 *
 * Assumptions:
 *   This function will *not* be called from an interrupt handler.
 *
 ************************************************************************************/

static int usbhost_transfer(FAR struct usbhost_driver_s *drvr,
                            FAR struct usbhost_epdesc_s *ed,
                            FAR uint8_t *buffer, size_t buflen)
{
# warning "Not Implemented"
  return -ENOSYS;
}

/************************************************************************************
 * Name: usbhost_disconnect
 *
 * Description:
 *   Called by the class when an error occurs and driver has been disconnected.
 *   The USB host driver should discard the handle to the class instance (it is
 *   stale) and not attempt any further interaction with the class driver instance
 *   (until a new instance is received from the create() method).  The driver
 *   should not called the class' disconnected() method.
 *
 * Input Parameters:
 *   drvr - The USB host driver instance obtained as a parameter from the call to
 *      the class create() method.
 *
 * Returned Values:
 *   None
 *
 * Assumptions:
 *   This function will *not* be called from an interrupt handler.
 *
 ************************************************************************************/

static void usbhost_disconnect(FAR struct usbhost_driver_s *drvr)
{
# warning "Not Implemented"
}
  
/*******************************************************************************
 * Initialization
 *******************************************************************************/

static void usbhost_tdinit(volatile struct usbhost_hctd_s *td)
{
# warning "Not Implemented"
}

static void usbhost_edinit(volatile struct usbhost_hced_s *ed)
{
# warning "Not Implemented"
}

static void usbhost_hccainit(volatile struct usbhost_hcca_s *hcca)
{
# warning "Not Implemented"
}

/*******************************************************************************
 * Public Functions
 *******************************************************************************/

/*******************************************************************************
 * Name: up_usbhostinitialize
 *
 * Description:
 *   Initialize USB host device controller hardware.
 *
 * Assumptions:
 *   This function is called very early in the initialization sequence in order
 *   to initialize the USB device functionality.
 *
 *******************************************************************************/

void up_usbhostinitialize(void)
{
  struct lpc17_usbhost_s *priv = &g_usbhost;
  uint32_t regval;
  irqstate_t flags;
  int i;

  usbtrace(TRACE_DEVINIT, 0);

  /* Enable power by setting PCUSB in the PCONP register */

  flags   = irqsave();
  regval  = lpc17_getreg(LPC17_SYSCON_PCONP);
  regval |= SYSCON_PCONP_PCUSB;
  lpc17_putreg(regval, LPC17_SYSCON_PCONP);

  /* Enable clocking on USB (USB PLL clocking was initialized in very low-
   * evel clock setup logic (see lpc17_clockconfig.c)).  We do still need
   * to set up USBOTG CLKCTRL to enable clocking.
   */

  lpc17_putreg(LPC17_CLKCTRL_ENABLES, LPC17_USBOTG_CLKCTRL);

  /* Then wait for the clocks to be reported as "ON" */

  do
    {
      regval = lpc17_getreg(LPC17_USBOTG_CLKST);
    }
  while ((regval & LPC17_CLKCTRL_ENABLES) != LPC17_CLKCTRL_ENABLES);

  lpc17_putreg(3, LPC17_USBOTG_STCTRL);

  /* Step 3: Configure I/O pins */

  usbhost_dumpgpio();
  lpc17_configgpio(GPIO_USB_DP);      /* Positive differential data */
  lpc17_configgpio(GPIO_USB_DM);      /* Negative differential data */
  lpc17_configgpio(GPIO_USB_UPLED);   /* GoodLink LED control signal */
  lpc17_configgpio(GPIO_USB_PPWR);    /* Port Power enable signal for USB port */
  lpc17_configgpio(GPIO_USB_PWRD);    /* Power Status for USB port (host power switch) */
  lpc17_configgpio(GPIO_USB_OVRCR);   /* USB port Over-Current status */
  usbhost_dumpgpio();

  udbg("Initializing Host Stack\n");

  /* Initialize all the TDs, EDs and HCCA to 0 */

  usbhost_edinit(EDCtrl);
  usbhost_edinit(EDBulkIn);
  usbhost_edinit(EDBulkOut);
  usbhost_tdinit(TDHead);
  usbhost_tdinit(TDTail);
  usbhost_hccainit(Hcca);

  /* Wait 50MS then perform hardware reset */

  up_mdelay(50);

  lpc17_putreg(0, LPC17_USBHOST_CTRL);        /* Hardware reset */
  lpc17_putreg(0, LPC17_USBHOST_CTRLHEADED);  /* Initialize control list head to Zero */
  lpc17_putreg(0, LPC17_USBHOST_BULKHEADED);  /* Initialize bulk list head to Zero */

  /* Software reset */

  lpc17_putreg(OHCI_CMDST_HCR, LPC17_USBHOST_CMDST);
  
  /* Write Fm interval and largest data packet counter */
  
  lpc17_putreg(DEFAULT_FMINTERVAL, LPC17_USBHOST_FMINT);

  /* Put HC in operational state */

  regval  = lpc17_getreg(LPC17_USBHOST_CTRL);
  regval &= ~OHCI_CTRL_HCFS_MASK;
  regval |= OHCI_CTRL_HCFS_OPER;
  lpc17_putreg(regval, LPC17_USBHOST_CTRL);

  /* Set global power */

  lpc17_putreg(OHCI_RHSTATUS_LPS, LPC17_USBHOST_RHSTATUS);

  /* Set HCCA base address */

  lpc17_putreg((uint32_t)Hcca, LPC17_USBHOST_HCCA);

  /* Clear pending interrupts */

  regval = lpc17_getreg(LPC17_USBHOST_INTST);
  lpc17_putreg(regval, LPC17_USBHOST_INTST);

  /* Enable OHCI interrupts */
 
  lpc17_putreg((OHCI_INT_MIE | OHCI_INT_WDH | OHCI_INT_RHSC), LPC17_USBHOST_INTEN);

  /* Attach USB host controller interrupt handler */

  if (irq_attach(LPC17_IRQ_USB, lpc17_usbinterrupt) != 0)
    {
      usbtrace(TRACE_DEVERROR(LPC17_TRACEERR_IRQREGISTRATION),
               (uint16_t)LPC17_IRQ_USB);
      return;
    }

  /* Enable USB interrupts at the SYCON controller */

  flags = irqsave();
  regval = lpc17_getreg(LPC17_SYSCON_USBINTST);
  regval |= SYSCON_USBINTST_ENINTS;
  lpc17_putreg(regval, LPC17_SYSCON_USBINTST);
  irqrestore(flags);

  /* Enable interrupts at the interrupt controller */

  up_enable_irq(LPC17_IRQ_USB); /* enable USB interrupt */
  udbg("USB host Initialized\n");
}
