/*
 * Frozen OSv configuration.
 *
 * Formerly this was kconfig-generated into ~35 one-macro-per-file headers
 * (kernel_config_<option>.h). Those are gone; this single header is the whole
 * compile-time configuration surface. Each macro is #ifndef-guarded so the
 * Makefile can still override any value with -DCONF_<option>=... Build-time
 * settings (compiler flags, driver set, version script) live in conf/config.mk,
 * not here.
 */
#ifndef OSV_KERNEL_CONFIG_H
#define OSV_KERNEL_CONFIG_H

#ifndef CONF_core_debug_buffer_size
#define CONF_core_debug_buffer_size 0xc800
#endif
#ifndef CONF_core_dynamic_percpu_size
#define CONF_core_dynamic_percpu_size 65536
#endif
#ifndef CONF_core_rcu_defer_queue_size
#define CONF_core_rcu_defer_queue_size 2000
#endif
#ifndef CONF_fs_max_file_descriptors
#define CONF_fs_max_file_descriptors 0x4000
#endif
#ifndef CONF_interrupt_stack_size
#define CONF_interrupt_stack_size 0x1000
#endif
#ifndef CONF_lazy_stack
#define CONF_lazy_stack 0
#endif
#ifndef CONF_lazy_stack_invariant
#define CONF_lazy_stack_invariant 0
#endif
#ifndef CONF_logger_debug
#define CONF_logger_debug 0
#endif
#ifndef CONF_memory_debug
#define CONF_memory_debug 0
#endif
#ifndef CONF_memory_l1_pool_size
#define CONF_memory_l1_pool_size 512
#endif
#ifndef CONF_memory_page_batch_size
#define CONF_memory_page_batch_size 32
#endif
#ifndef CONF_preempt
#define CONF_preempt 1
#endif
#ifndef CONF_threads_default_exception_stack_size
#define CONF_threads_default_exception_stack_size 0x10000
#endif
#ifndef CONF_threads_default_kernel_stack_size
#define CONF_threads_default_kernel_stack_size 65536
#endif
#ifndef CONF_threads_default_pthread_stack_size
#define CONF_threads_default_pthread_stack_size 0x100000
#endif
#ifndef CONF_tracepoints
#define CONF_tracepoints 1
#endif
#ifndef CONF_tracepoints_strace
#define CONF_tracepoints_strace 1
#endif
#ifndef CONF_tracepoints_sampler
#define CONF_tracepoints_sampler 1
#endif

#endif /* OSV_KERNEL_CONFIG_H */
