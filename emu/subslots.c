
#include <stdio.h>
#include "subslots.h"
#include "pico.h"

uint8_t __not_in_flash_func(subslots_read)(void* context, uint16_t address)
{
    if (address == 0xffff) {
        return ~(((subslot_context_t *)context)->subslot_register);
    } 
    else 
    {
        uint32_t ss = (((subslot_context_t *)context)->subslot_register >> (address >> 14) * 2) & 3;
        memory_context_t* mc = &((subslot_context_t *)context)->subslots[ss];
        return (*mc->read_func)(mc->context, address);
    }
}

void __not_in_flash_func(subslots_write)(void* context, uint16_t address, uint8_t value)
{
    if (address == 0xffff) {
#ifdef SLOT_DEBUG
        fprintf(stderr, "[subslot] reg %02X -> %02X\n",
                ((subslot_context_t *)context)->subslot_register, value);
#endif
        ((subslot_context_t *)context)->subslot_register = value;
    } 
    else 
    {
        uint32_t ss = (((subslot_context_t *)context)->subslot_register >> (address >> 14) * 2) & 3;
        memory_context_t* mc = &((subslot_context_t *)context)->subslots[ss];
        (*mc->write_func)(mc->context, address, value);
    }
}

void subslots_add_subslot(subslot_context_t *subslot_context, uint32_t subslot, void *context, read_byte_t read_func, write_byte_t write_func) 
{
    subslot_context->subslots[subslot & 3].context = context;
    subslot_context->subslots[subslot & 3].read_func = read_func;
    subslot_context->subslots[subslot & 3].write_func = write_func;
}