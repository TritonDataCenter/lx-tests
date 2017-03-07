/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

/*
 * Copyright (c) 2016, Joyent, Inc.
 */


#define ASM_ENTRY_ALIGN 8
#define ENTRY_NP(x)		\
.text;				\
.align	ASM_ENTRY_ALIGN;	\
.globl	x;			\
.type	x, @function;		\
x:

#define SET_SIZE(x)		\
.size	x, [.-x]


ENTRY_NP(run_stack_func)

	# Setup the new stack frame based on the provided pointer and budget.
	# This includes an additional 8 bytes the function call return address.
	# The original stack pointer is stashed immediately above that.
	addq	%rsi, %rdi
	addq	$0x8, %rdi
	movq	%rsp, 0x8(%rdi)
	movq	%rdi, %rsp

	# Stage the function to be called
	movq	%rdx, %rax
	movq	(%rcx), %rdi
	movq	0x8(%rcx), %rsi

	# Execute!
	callq	*%rax

	# Clean up and return to our original stack
	movq	0x8(%rsp), %rsp
	retq
SET_SIZE(run_stack_func)
