/* The setup button: one GPIO, polled, debounced, reporting long presses.
 *
 * Wiring (ARCHITECTURE.md's "Setup button" table is the authority, and is
 * where someone assembling the gadget will look): one leg to D1 / GPIO3, the
 * other to GND, with no resistor of your own. The internal pull-up does the rest, which makes the
 * pin read 1 when released and 0 when pressed -- backwards from intuition, and
 * the reason button.c says so more than once.
 *
 * D1 was chosen because it has no strapping role (unlike GPIO2, GPIO8 and
 * GPIO9) and is not the serial console (unlike GPIO20/21), so a button held
 * down across a reset cannot change how the chip boots.
 *
 * This module deliberately does NOT touch the display. token_monitor.c hands
 * ownership of the panel to usage_task once it is created, and nothing else
 * may draw without a mutex that does not exist. So the button reports, and
 * usage_task decides what that should look like.
 */
#pragma once

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

/* How long the button must be held before it counts. Deliberately long: this
 * is a thing that sits on a desk, and a gadget that reconfigures itself on an
 * accidental brush is a bad gadget. */
#define BUTTON_HOLD_MS 3000

/* Configure the pin and start the polling task.
 *
 * On each completed long press, `long_press_bit` is set in `events`. The
 * caller owns that group and is responsible for clearing the bit once it has
 * acted on it; this module only ever sets it.
 *
 * One press sets the bit once. Holding the button down longer does not set it
 * repeatedly -- the next one cannot happen until the button has been released
 * and pressed again.
 *
 * Call after the event group exists. Safe to call before or after the task
 * that consumes the bit is created: a bit set early simply waits there.
 */
void button_start(EventGroupHandle_t events, EventBits_t long_press_bit);
