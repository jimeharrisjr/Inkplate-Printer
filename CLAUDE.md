## Project

The goal of this project is to turn the Inkplate6 and Inkplate6 Color (https://docs.soldered.com/inkplate/) into network "printers," such that a Mac or Windows computer on the network can discover the Inkplate as a printer, and print to it (display documents on the inkplate). The documents should be stored on the SD card in the Inkplate. The touch switches on the inkplate (https://inkplate.readthedocs.io/en/stable/examples.html) can page forward, page back, or go to the next document (if any).

The project is intended to be a C++ program using the Arduino libraries available for the Inkplate and other example libraries for Arduino as needed to fulfill the needs outlined in the "Flow" section below.

## Flow

When the Inkplate is first powered on, if it has no WiFi credentials stored, or if no AP matches the stored credentials, it should configure itself as a WiFi AP serving a simple web page allowing someone to connect with a phone or computer, and either input new WiFi credentials, or click a button to continue without network (only able to read documents already stored).

If the Inkplate has network credentials, it should connect to the network and do the following:

* mDNS Advertising: Use the standard ESPmDNS library to broadcast the _ipp._tcp service. This will make the Inkplate show up in the "Add Printer" dialogs on Mac and Windows.
* The IPP Server: You will need to write or adapt a lightweight HTTP server on the ESP32 to handle IPP POST requests.
* Format Forcing (PWG Raster): In your IPP capabilities response, you must specify that your printer only accepts image/pwg-raster or image/urf (Apple Raster) so the sending computer does the heavy rendering on their end.
* Store incoming documents on the SD card, and render the most recent page of the most recent document printed on the e-ink screen.
* Allow for navication through documents with the touch switches as outlined above.

## Tasks

First, build out a development plan for this project, with the understanding that it will be loaded into the InkPlate using the Arduino IDE.

Put any questions into a separate markdown document so I can respond inline.

 
