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

/* Implementation of a logical timer for rockpro64
 *
 * We use the 1 rk for timeouts and 1 for a millisecond tick.
 */
#include <platsupport/timer.h>
#include <platsupport/ltimer.h>
#include <platsupport/plat/timer.h>
#include <utils/util.h>

#define NUM_RK 2
#define RK_ID RKTIMER0

typedef struct {
    rk_t rks[NUM_RK];
    void *vaddrs[NUM_RK];
    ps_io_ops_t ops;
} rk_ltimer_t;

static size_t get_num_irqs(void *data)
{
    return NUM_RK;
}

static int get_nth_irq(void *data, size_t n, ps_irq_t *irq)
{
    assert(n < get_num_irqs(data));

    irq->type = PS_INTERRUPT;
    irq->irq.number = rk_irq(n + RK_ID);
    return 0;
}

static size_t get_num_pmems(void *data)
{
    return NUM_RK;
}

static int get_nth_pmem(void *data, size_t n, pmem_region_t *region)
{
    assert(n < NUM_RK);
    region->length = PAGE_SIZE_4K;
    region->base_addr = (uintptr_t) rk_paddr(n + RK_ID);
    return 0;
}

static int handle_irq(void *data, ps_irq_t *irq)
{
    assert(data != NULL);
    rk_ltimer_t *rk_ltimer = data;
    if (irq->irq.number == RKTIMER1_INTERRUPT) {
        rk_handle_irq(&rk_ltimer->rks[TIMEOUT_RK]);
        return 0;
    }

    if (irq->irq.number == RKTIMER0_INTERRUPT) {
        rk_handle_irq(&rk_ltimer->rks[TIMER_RK]);
        return 0;
    }

    ZF_LOGE("Unknown irq");
    return EINVAL;
}

static int get_time(void *data, uint64_t *time)
{
    assert(data != NULL);
    assert(time != NULL);

    rk_ltimer_t *rk_ltimer = data;
    *time = rk_get_time(&rk_ltimer->rks[TIMER_RK]);
    return 0;
}

static int get_resolution(void *data, uint64_t *resolution)
{
    ZF_LOGF("Not implemented");
    return 0;
}

static int set_timeout(void *data, uint64_t ns, timeout_type_t type)
{
    assert(data != NULL);
    rk_ltimer_t *rk_ltimer = data;

    if (type == TIMEOUT_ABSOLUTE) {
        uint64_t current_time = 0;
        int error = get_time(data, &current_time);
        assert(error == 0);
        ns -= current_time;
    }

    return rk_set_timeout(&rk_ltimer->rks[TIMEOUT_RK], ns, type == TIMEOUT_PERIODIC);
}

static int reset(void *data)
{
    assert(data != NULL);
    rk_ltimer_t *rk_ltimer = data;
    /* just reset the timeout timer */
    rk_stop(&rk_ltimer->rks[TIMEOUT_RK]);
    return 0;
}

static void destroy(void *data)
{
    assert(data);

    rk_ltimer_t *rk_ltimer = data;

    for (int i = 0; i < NUM_RK; i++) {
        if (rk_ltimer->vaddrs[i]) {
            rk_stop(&rk_ltimer->rks[i]);
            ps_io_unmap(&rk_ltimer->ops.io_mapper, rk_ltimer->vaddrs[i], PAGE_SIZE_4K);
        }
    }

    ps_free(&rk_ltimer->ops.malloc_ops, sizeof(rk_ltimer), rk_ltimer);
}

int ltimer_default_init(ltimer_t *ltimer, ps_io_ops_t ops)
{

    int error = ltimer_default_describe(ltimer, ops);
    if (error) {
        return error;
    }

    ltimer->handle_irq = handle_irq;
    ltimer->get_time = get_time;
    ltimer->get_resolution = get_resolution;
    ltimer->set_timeout = set_timeout;
    ltimer->reset = reset;
    ltimer->destroy = destroy;

    error = ps_calloc(&ops.malloc_ops, 1, sizeof(rk_ltimer_t), &ltimer->data);
    if (error) {
        return error;
    }
    assert(ltimer->data != NULL);
    rk_ltimer_t *rk_ltimer = ltimer->data;
    rk_ltimer->ops = ops;

    /* map the frame we need */
    pmem_region_t region;
    error = get_nth_pmem(ltimer->data, 0, &region);
    assert(error == 0);
    void * base = ps_pmem_map(&ops, region, false, PS_MEM_NORMAL);
    if (rk_ltimer->vaddrs[0] == NULL) {
        error = -1;
    }

    rk_config_t config = {
        .vaddr = base,
        .id = RK_ID
    };
    error = rk_init(&rk_ltimer->rks[TIMER_RK], config);
    
    rk_config_t config1 = {
        .vaddr = (base + 0x20),
        .id = RK_ID + 1
    };
    error = rk_init(&rk_ltimer->rks[TIMEOUT_RK], config1);

    /* start the timer rk */
    if (!error) {
       error = rk_start(&rk_ltimer->rks[TIMER_RK], TIMER_RK);
    }

    if (error) {
        destroy(rk_ltimer);
    }

    return error;
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
