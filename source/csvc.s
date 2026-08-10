@   svcControlService (syscall 0xB0) -- not part of libctru at all (it's an
@   undocumented syscall Luma3DS's kernel patches expose to userland, used
@   by saves.c's GBA Virtual Console save-reading path to "steal" a client
@   session to the PxiFS0 service). libctru only declares/implements the
@   syscalls it officially supports, so the raw stub has to be provided
@   here instead of just an extern C prototype -- otherwise the prototype
@   compiles fine but the linker has nothing to resolve it against.
@
@   Ported verbatim from Checkpoint's 3ds/source/csvc.s, itself adapted
@   from Luma3DS (AuroraWright/Luma3DS), under the terms below.

@   This paricular file is licensed under the following terms:

@   This software is provided 'as-is', without any express or implied warranty. In no event will the authors be held liable
@   for any damages arising from the use of this software.
@
@   Permission is granted to anyone to use this software for any purpose, including commercial applications, and to alter it
@   and redistribute it freely, subject to the following restrictions:
@
@    The origin of this software must not be misrepresented; you must not claim that you wrote the original software.
@    If you use this software in a product, an acknowledgment in the product documentation would be appreciated but is not required.
@
@    Altered source versions must be plainly marked as such, and must not be misrepresented as being the original software.
@    This notice may not be removed or altered from any source distribution.

.arm
.balign 4

.macro SVC_BEGIN name
    .section .text.\name, "ax", %progbits
    .global \name
    .type \name, %function
    .align 2
    .cfi_startproc
\name:
.endm

.macro SVC_END
    .cfi_endproc
.endm

SVC_BEGIN svcControlService
    svc 0xB0
    bx lr
SVC_END
