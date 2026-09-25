/* SPDX-License-Identifier: MIT */
.section .rodata.ipc_child,"a",@progbits
.global ipc_child_image, ipc_child_image_end
ipc_child_image:
.incbin IPC_TEST_CHILD_IMAGE
ipc_child_image_end:
.section .note.GNU-stack,"",@progbits
