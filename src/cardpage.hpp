#pragma once

/** What other parts of the UI need from the SD Card page */

#include "cardio.hpp"

/** A slot on the card being laid out */
osiris::Slot *cardSlot(int bank, int slot);
/** Saves the card so the next bulk change can be undone with the page's Undo button */
void cardSnapshot(const char *label);
void cardSetStatus(const std::string &status);
/** Up to `count` free slots, starting at the selected slot and then from A1 */
std::vector<osiris::Position> cardFreeSlots(int count);
/** Sets a bank's pack name if it doesn't have one yet */
void cardSetPackIfEmpty(int bank, const std::string &pack);
