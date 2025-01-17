/*
 * Copyright 2019, Data61
 * Commonwealth Scientific and Industrial Research Organisation (CSIRO)
 * ABN 41 687 119 230.
 *
 * This software may be distributed and modified according to the terms of
 * the BSD 2-Clause license. Note that NO WARRANTY is provided.
 * See "LICENSE_BSD2.txt" for details.
 *
 * @TAG(DATA61_BSD)
 */
/* Implementation of a logical timer for HiFive Unleashed platform.
 *
 * We use two pwms: one for the time and the other for timeouts.
 */
#include <platsupport/timer.h>
#include <platsupport/ltimer.h>
#include <platsupport/plat/pwm.h>
#include <platsupport/pmem.h>
#include <utils/util.h>

typedef struct {
    pwm_t pwm;
    void *vaddr;
} pwm_ltimer_t;

typedef struct {
    pwm_ltimer_t counter;
    pwm_ltimer_t timeouts;
    ps_io_ops_t ops;
} hifive_timers_t;

static ps_irq_t irqs[] = {
    {
        .type = PS_INTERRUPT,
        .irq.number = PWM0_INTERRUPT0

    },
    {
        .type = PS_INTERRUPT,
        .irq.number = PWM1_INTERRUPT0
    },
};

static pmem_region_t pmems[] = {
    {
        .type = PMEM_TYPE_DEVICE,
        .base_addr = PWM0_PADDR,
        .length = PAGE_SIZE_4K
    },
    {
        .type = PMEM_TYPE_DEVICE,
        .base_addr = PWM1_PADDR,
        .length = PAGE_SIZE_4K
    }
};

#define N_IRQS ARRAY_SIZE(irqs)
#define N_PMEMS ARRAY_SIZE(pmems)

 size_t get_num_irqs(void *data)
{
    return N_IRQS;
}

static int get_nth_irq(void *data, size_t n, ps_irq_t *irq)
{
    assert(n < N_IRQS);

    *irq = irqs[n];
    return 0;
}

static size_t get_num_pmems(void *data)
{
    return N_PMEMS;
}

static int get_nth_pmem(void *data, size_t n, pmem_region_t *paddr)
{
    assert(n < N_PMEMS);
    *paddr = pmems[n];
    return 0;
}

static int handle_irq(void *data, ps_irq_t *irq)
{
    assert(data != NULL);
    hifive_timers_t *timers = data;
    long irq_number = irq->irq.number;
    if (irq_number == PWM0_INTERRUPT0) {
        pwm_handle_irq(&timers->counter.pwm, irq->irq.number);
    } else if (irq_number == PWM1_INTERRUPT0) {
        pwm_handle_irq(&timers->timeouts.pwm, irq->irq.number);
    } else {
        ZF_LOGE("Invalid IRQ number: %d received.\n", irq_number);
    }
    return 0;
}

static int get_time(void *data, uint64_t *time)
{
    assert(data != NULL);
    assert(time != NULL);
    hifive_timers_t *timers = data;

    *time = pwm_get_time(&timers->counter.pwm);
    return 0;
}

static int get_resolution(void *data, uint64_t *resolution)
{
    return ENOSYS;
}

static int set_timeout(void *data, uint64_t ns, timeout_type_t type)
{
    assert(data != NULL);
    hifive_timers_t *timers = data;

    switch (type) {
    case TIMEOUT_ABSOLUTE: {
        uint64_t time = pwm_get_time(&timers->counter.pwm);
        if (time >= ns) {
            return ETIME;
        }
        return pwm_set_timeout(&timers->timeouts.pwm, ns - time, false);
    }
    case TIMEOUT_RELATIVE:
        return pwm_set_timeout(&timers->timeouts.pwm, ns, false);
    case TIMEOUT_PERIODIC:
        return pwm_set_timeout(&timers->timeouts.pwm, ns, true);
    }

    return EINVAL;
}

static int reset(void *data)
{
    assert(data != NULL);
    hifive_timers_t *timers = data;
    pwm_stop(&timers->counter.pwm);
    pwm_start(&timers->counter.pwm);
    pwm_stop(&timers->timeouts.pwm);
    pwm_start(&timers->timeouts.pwm);
    return 0;
}

static void destroy(void *data)
{
    assert(data);
    hifive_timers_t *timers = data;
    if (timers->counter.vaddr) {
        pwm_stop(&timers->counter.pwm);
        ps_pmem_unmap(&timers->ops, pmems[0], timers->counter.vaddr);
    }
    if (timers->timeouts.vaddr) {
        pwm_stop(&timers->timeouts.pwm);
        ps_pmem_unmap(&timers->ops, pmems[0], timers->timeouts.vaddr);
    }
    ps_free(&timers->ops.malloc_ops, sizeof(timers), timers);
}

static int create_ltimer(ltimer_t *ltimer, ps_io_ops_t ops)
{
    assert(ltimer != NULL);
    ltimer->handle_irq = handle_irq;
    ltimer->get_time = get_time;
    ltimer->get_resolution = get_resolution;
    ltimer->set_timeout = set_timeout;
    ltimer->reset = reset;
    ltimer->destroy = destroy;

    int error = ps_calloc(&ops.malloc_ops, 1, sizeof(hifive_timers_t), &ltimer->data);
    if (error) {
        return error;
    }
    assert(ltimer->data != NULL);

    return 0;
}

static int init_ltimer(ltimer_t *ltimer)
{
    assert(ltimer != NULL);
    hifive_timers_t *timers = ltimer->data;

    /* setup pwm */
    pwm_config_t config_counter = {
        .vaddr = timers->counter.vaddr,
        .mode = UPCOUNTER,
    };
    pwm_config_t config_timeout = {
        .vaddr = timers->timeouts.vaddr,
        .mode = TIMEOUT,
    };

    pwm_init(&timers->counter.pwm, config_counter);
    pwm_init(&timers->timeouts.pwm, config_timeout);
    pwm_start(&timers->counter.pwm);
    return 0;
}

int ltimer_default_init(ltimer_t *ltimer, ps_io_ops_t ops)
{

    int error = ltimer_default_describe(ltimer, ops);
    if (error) {
        return error;
    }

    error = create_ltimer(ltimer, ops);
    if (error) {
        return error;
    }

    hifive_timers_t *timers = ltimer->data;
    timers->ops = ops;
    timers->counter.vaddr = ps_pmem_map(&ops, pmems[0], false, PS_MEM_NORMAL);
    if (timers->counter.vaddr == NULL) {
        destroy(ltimer->data);
    }
    timers->timeouts.vaddr = ps_pmem_map(&ops, pmems[1], false, PS_MEM_NORMAL);
    if (timers->timeouts.vaddr == NULL) {
        destroy(ltimer->data);
    }

    init_ltimer(ltimer);
    if (error) {
        return error;
    }

    /* success! */
    return 0;
}

int ltimer_default_describe(ltimer_t *ltimer, ps_io_ops_t ops)
{
    if (ltimer == NULL) {
        ZF_LOGE("Timer is NULL!");
        return EINVAL;
    }

    ltimer->get_num_irqs = get_num_irqs;
    ltimer->get_nth_irq = get_nth_irq;
    ltimer->get_num_pmems = get_num_pmems;
    ltimer->get_nth_pmem = get_nth_pmem;
    return 0;
}
