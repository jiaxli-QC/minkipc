// Copyright (c) 2025, Qualcomm Innovation Center, Inc. All rights reserved.
// SPDX-License-Identifier: BSD-3-Clause

#if defined(__aarch64__)
      .global    recv_ioctl
      .global    recv_skip
      .global    recv
      .p2align   4
recv_ioctl:
      ldr     w4, [x3]
      cbnz    w4, recv_skip
      mov     x8, 29 // ioctl syscall is identified by 29 on ARM
recv: svc     0
      ret
recv_skip:
      mov     x0, -4 // -EINTR
      ret
 
      .end
#endif
