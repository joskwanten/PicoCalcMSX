#include <stdio.h>
#include "slots.h"
#include "pico.h"

uint8_t __not_in_flash_func(slots_read)(void* slots_context, uint16_t address) {
    //  printf("r: %x\n", address);
    uint32_t page = address >> 14;
    uint32_t selected_slot = ((slots_context_t *) slots_context)->selected_slots[page];
    memory_context_t* slot = &((slots_context_t *) slots_context)->slots[selected_slot];
    return (*slot->read_func)(slot->context, address);
}

void __not_in_flash_func(slots_write)(void* slots_context, uint16_t address, uint8_t value) {
    //printf("w: %x\n", address);
    uint32_t page = address >> 14;
    uint32_t selected_slot = ((slots_context_t *) slots_context)->selected_slots[page];
    memory_context_t* slot = &((slots_context_t *) slots_context)->slots[selected_slot];
    // printf("w: %x, slot: %x\n", address, selected_slot);
    (*slot->write_func)(slot->context, address, value);
}

void slots_set_slot_register(slots_context_t* slots_context, uint8_t value) {
#ifdef SLOT_DEBUG
    fprintf(stderr, "[a8] %02X (p0=%d p1=%d p2=%d p3=%d)\n", value,
            value & 3, (value >> 2) & 3, (value >> 4) & 3, (value >> 6) & 3);
#endif
    slots_context->selected_slots[0] = value & 3;
    slots_context->selected_slots[1] = (value >> 2) & 3;
    slots_context->selected_slots[2] = (value >> 4) & 3;
    slots_context->selected_slots[3] = (value >> 6) & 3;
}

uint8_t slots_get_slot_register(slots_context_t* slots_context) {
    // printf("Reading slot register %x %x %x %x\n", slots_context->selected_slots[0], 
    //     slots_context->selected_slots[1], 
    //     slots_context->selected_slots[2], 
    //     slots_context->selected_slots[3]);

    return (slots_context->selected_slots[3] << 6) | (slots_context->selected_slots[2] << 4) |
        (slots_context->selected_slots[1] << 2) |(slots_context->selected_slots[0]);
}

void slots_add_slot(slots_context_t* slots_context, uint32_t slot, void* context, read_byte_t read_func, write_byte_t write_func) {
    slots_context->slots[slot & 3].context = context;
    slots_context->slots[slot & 3].read_func = read_func;
    slots_context->slots[slot & 3].write_func = write_func;
}