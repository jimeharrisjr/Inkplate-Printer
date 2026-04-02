# Questions

Please respond inline below each question.

## Hardware Target

1. **Which Inkplate model do you have?** The Inkplate 6 (800x600, monochrome/3-bit grayscale), Inkplate 6 Plus (1024x758, grayscale + capacitive touchscreen), and Inkplate 6 Color (600x448, 7-color) all have different display resolutions and capabilities. Which one(s) should I target first?

ANSWER: I'd like it to work for either Inkplate 6 or Inkplate 6 color. Ideally, it would check which hardware it has and adjust, but if that isn't feasible, at load time in the Arduino IDE, a configuration file that lets you choose which model to configure for would be acceptable

2. **Touch switches vs touchscreen:** The original Inkplate 6 has 3 capacitive touch *pads* (not a touchscreen). The Inkplate 6 Plus has a full capacitive touchscreen. Which input method are you using? (This affects navigation UI design.)

ANSWER: right now, let's use the touch switches

3. **SD card:** Is an SD card already inserted and formatted? Any preference on filesystem (FAT32 is the default and most compatible)?

ANSWER: A formatted FAT-32 card will be already inserted

## Network & Discovery

4. **Printer name:** What should the printer identify itself as in the network printer list? Something like "Inkplate E-Ink Display"? Or should it use a configurable hostname?

ANSWER: Let's make it a configurable item at the time the sketch is loaed, with a default of Inkplate-printer'

5. **Security:** Should the IPP server require any authentication, or is open access on the local network acceptable?

ANSWER: Open access is acceptable

6. **IPP vs IPPS:** Standard IPP uses port 631 (unencrypted). IPPS uses TLS. Modern macOS and Windows increasingly prefer IPPS. However, TLS on ESP32 adds significant memory overhead. Is unencrypted IPP acceptable for your use case?

ANSWER: Unencrypted is acceptable

## Print Job Handling

7. **Resolution:** The sending computer will rasterize documents before sending. What DPI should we advertise? Lower DPI = smaller data transfer = faster printing. Given the Inkplate6's 800x600 physical pixels, advertising a matching resolution (e.g., 150 DPI for ~5.3" x 4" printable area) makes sense. Any preference?

ANSWER: Match the inkplate resolution, as suggested

8. **Color handling:** For the monochrome Inkplate 6, incoming print data needs to be converted to grayscale/dithered. Should I implement Floyd-Steinberg dithering for best visual quality, or simple threshold for speed?

ANSWER: If possible, implement Floyd-Steinberg

9. **Multi-page documents:** When a multi-page document is printed, should the display immediately show page 1 of the new document, or stay on whatever was being viewed and just notify that a new document arrived?

ANSWER: The screen should stay on whatever was being viewed and just notify that a new document arrived

## Storage & Navigation

10. **Storage limits:** Should there be a maximum number of stored documents or a storage limit? When the SD card fills up, should the oldest documents be automatically deleted, or should printing fail with an error?

ANSWER: When filled up, delete the oldest document

11. **Navigation model:** You mentioned three touch switches for forward/back/next-document. Should "next document" cycle back to the first document after the last one? Should there be any way to delete documents from the device itself?

ANSWER: Cycle through, but have a feature that deletes the current document if all three buttons are held down for a few seconds after confirming with a notice and a single button press

## Power Management

12. **Power source:** Will this be running on USB power continuously, or on battery? If battery, deep sleep between interactions becomes important and affects the architecture significantly.

ANSWER: It will be battery powered, but the idea is it will be turned off with the toggle switch on the side when not in use.

13. **Display refresh:** E-ink displays have limited refresh cycles and can show ghosting with partial updates. Should I use full refresh every time (slower, ~2 seconds, no ghosting) or partial refresh for navigation (faster, ~0.3 seconds, some ghosting)?

ANSWER: When switching between different documents, always do a full refresh. If the user is switching pages rapidly (multiple, quick button presses) do a partial refresh, and once they stop on a page for more than a second, do a full refresh.
