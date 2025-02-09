/****************************************************************************
 * drivers/misc/rpmsgdev.h
 *
 * Licensed to the Apache Software Foundation (ASF) under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.  The
 * ASF licenses this file to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance with the
 * License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.  See the
 * License for the specific language governing permissions and limitations
 * under the License.
 *
 ****************************************************************************/

#ifndef __DRIVERS_MISC_RPMSGDEV_H
#define __DRIVERS_MISC_RPMSGDEV_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/compiler.h>

/****************************************************************************
 * Pre-processor definitions
 ****************************************************************************/

#ifndef ARRAY_SIZE
#  define ARRAY_SIZE(x)         (sizeof(x) / sizeof((x)[0]))
#endif

#define RPMSGDEV_NAME_PREFIX     "rpmsgdev-"
#define RPMSGDEV_NAME_PREFIX_LEN 9

#define RPMSGDEV_OPEN            1
#define RPMSGDEV_CLOSE           2
#define RPMSGDEV_READ            3
#define RPMSGDEV_WRITE           4
#define RPMSGDEV_LSEEK           5
#define RPMSGDEV_IOCTL           6

/****************************************************************************
 * Public Types
 ****************************************************************************/

begin_packed_struct struct rpmsgdev_header_s
{
  uint32_t                command;
  int32_t                 result;
  uint64_t                cookie;
} end_packed_struct;

begin_packed_struct struct rpmsgdev_open_s
{
  struct rpmsgdev_header_s header;
  int32_t                  flags;
} end_packed_struct;

begin_packed_struct struct rpmsgdev_close_s
{
  struct rpmsgdev_header_s header;
} end_packed_struct;

begin_packed_struct struct rpmsgdev_read_s
{
  struct rpmsgdev_header_s header;
  uint32_t                 count;
  char                     buf[1];
} end_packed_struct;

#define rpmsgdev_write_s rpmsgdev_read_s

begin_packed_struct struct rpmsgdev_lseek_s
{
  struct rpmsgdev_header_s header;
  int32_t                  whence;
  int32_t                  offset;
} end_packed_struct;

begin_packed_struct struct rpmsgdev_ioctl_s
{
  struct rpmsgdev_header_s header;
  int32_t                  request;
  uint64_t                 arg;
  uint32_t                 arglen;
  char                     buf[1];
} end_packed_struct;

/****************************************************************************
 * Internal function prototypes
 ****************************************************************************/

/****************************************************************************
 * Internal data
 ****************************************************************************/

#endif /* __DRIVERS_MISC_RPMSGDEV_H */
