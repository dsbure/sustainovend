# SustainoVend Arduino Mega + ESP32 (for wifi) code

`sustainovend.ino` is for the Arduino Mega.

`sustainovend-esp32.cpp` is for the ESP32 (should be compatible with the ESP8266 with minor modifications).

For the points and ID system to function, a Firebase project needs to be set up. See [here](https://github.com/mobizt/FirebaseClient#project-preparation-and-setup) for instructions.

You will need to replace both `YOUR_FIREBASE_PROJECT` and `YOUR_FIREBASE_CLIENT_EMAIL` in `sustainovend-esp32.cpp` and `YOUR_API_KEY` in `secrets.h` with the values provided in the provided json file.

Licensed under the GNU GPL 3.0