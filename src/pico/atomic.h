/* Copyright (c) 2007 Dean Camera
   All rights reserved.

   Adapted for Cortex-M3 by Ingo Korb
   Adapted for Cortex-M33 (RP2350 / Pico 2 W) for sd2iec_pico2w

   Redistribution and use in source and binary forms, with or without
   modification, are permitted provided that the following conditions are met:

   * Redistributions of source code must retain the above copyright
     notice, this list of conditions and the following disclaimer.

   * Redistributions in binary form must reproduce the above copyright
     notice, this list of conditions and the following disclaimer in
     the documentation and/or other materials provided with the
     distribution.

   * Neither the name of the copyright holders nor the names of
     contributors may be used to endorse or promote products derived
     from this software without specific prior written permission.

  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
  AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
  IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
  ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
  LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
  CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
  SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
  INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
  CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
  ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
  POSSIBILITY OF SUCH DAMAGE.
*/

/* atomic.h: ATOMIC_BLOCK/NONATOMIC_BLOCK for Cortex-M33 (RP2350)

   Ported near-verbatim from src/lpc17xx/atomic.h (Cortex-M3): the
   cpsid/cpsie i instructions and the PRIMASK register are unchanged
   between Armv7-M and Armv8-M base/mainline, so this works as-is on
   RP2350's Cortex-M33 cores.

   IMPORTANT (RP2350 is dual-core, LPC17xx was single-core): cpsid/cpsie i
   and PRIMASK only affect the *executing* core's interrupts. Here
   core 0 runs the IEC/PIO bus engine and core 1
   runs Wi-Fi/lwIP/the management HTTP server. This header is only for
   guarding core-0-local interrupt handlers (e.g. IEC IRQs) against
   core-0 mainline code. It provides no protection against core 1 -- any
   state shared between the two cores (FatFs volume, upload queue,
   config) must use hardware_sync.h spinlocks/mutexes instead, as noted.
*/

#ifndef _UTIL_ATOMIC_H_
#define _UTIL_ATOMIC_H_ 1

#if !defined(__DOXYGEN__)
/* Internal helper functions. */
static __inline__ unsigned int __iSeiRetVal(void)
{
    asm volatile("cpsie i");
    return 1;
}

static __inline__ unsigned int __iCliRetVal(void)
{
    asm volatile("cpsid i");
    return 1;
}

static __inline__ void __iSeiParam(const unsigned int *__s)
{
    __asm__ volatile ("cpsie i" ::: "memory");
    (void)__s;
}

static __inline__ void __iCliParam(const unsigned int *__s)
{
    __asm__ volatile ("cpsid i" ::: "memory");
    (void)__s;
}

static __inline__ void __iRestore(const unsigned int *__s)
{
    unsigned int s = *__s;
    __asm__ volatile ("MSR primask, %0" :: "r" (s) : "memory");
}

static __inline__ unsigned int __get_primask(void)
{
    unsigned int mask = 0;

    __asm__ volatile ("MRS %0, primask" : "=r" (mask) );
    return mask;
}
#endif	/* !__DOXYGEN__ */

#if defined(__DOXYGEN__)
#define ATOMIC_BLOCK(type)
#else
#define ATOMIC_BLOCK(type) for ( type, __ToDo = __iCliRetVal(); \
	                       __ToDo ; __ToDo = 0 )
#endif	/* __DOXYGEN__ */

#if defined(__DOXYGEN__)
#define NONATOMIC_BLOCK(type)
#else
#define NONATOMIC_BLOCK(type) for ( type, __ToDo = __iSeiRetVal(); \
	                          __ToDo ;  __ToDo = 0 )
#endif	/* __DOXYGEN__ */

#if defined(__DOXYGEN__)
#define ATOMIC_RESTORESTATE
#else
#define ATOMIC_RESTORESTATE unsigned int sreg_save \
        __attribute__((__cleanup__(__iRestore))) = __get_primask()
#endif	/* __DOXYGEN__ */

#if defined(__DOXYGEN__)
#define ATOMIC_FORCEON
#else
#define ATOMIC_FORCEON unsigned int sreg_save \
	__attribute__((__cleanup__(__iSeiParam))) = 0
#endif	/* __DOXYGEN__ */

#if defined(__DOXYGEN__)
#define NONATOMIC_RESTORESTATE
#else
#define NONATOMIC_RESTORESTATE unsigned int sreg_save \
         __attribute__((__cleanup__(__iRestore))) = __get_primask()
#endif	/* __DOXYGEN__ */

#if defined(__DOXYGEN__)
#define NONATOMIC_FORCEOFF
#else
#define NONATOMIC_FORCEOFF unsigned int sreg_save \
	__attribute__((__cleanup__(__iCliParam))) = 0
#endif	/* __DOXYGEN__ */

#endif
