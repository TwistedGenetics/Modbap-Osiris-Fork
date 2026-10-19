#pragma once

/** What other parts of the UI need from the SD Card page */

#include "cardio.hpp"

/** A slot on the card being laid out */
osiris::Slot *cardSlot(int bank, int slot);
/** Saves the card so the next bulk change can be undone with the page's Undo button */
void cardSnapshot(const char *label);
void cardSetStatus(const std::string &status);
