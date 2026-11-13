/* SPDX-License-Identifier: (MIT OR GPL-2.0-only)
 * Standard descriptor for an OFFLINE CPU-wait candidate, not permission to load.
 */
#include <linux/module.h>
#include <linux/vermagic.h>
extern int init_module(void);
extern void cleanup_module(void);
MODULE_INFO(vermagic,VERMAGIC_STRING);
MODULE_INFO(depends,"");
__visible struct module __this_module
 __attribute__((section(".gnu.linkonce.this_module"),used)) = {
 .name = KBUILD_MODNAME,
 .init = init_module,
 .exit = cleanup_module,
 .arch = MODULE_ARCH_INIT,
};
