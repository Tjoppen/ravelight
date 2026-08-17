# Ravelight

A project for making WS2812 LED strips audio reactive, wirelessly.

This project consits of two parts: the transmitter and the receiver.

The transmitter takes audio in, samples it, sets up a WiFi access point and broadcasts the sampled audio via UDP over WiFi.
The sampled audio gets its DC bias removed.
If there is no audio being played, the sender will transmit a test signal to keep the LEDs blinking.

The receiver connects to the access point and receives UDP packets containing audio.
The audio is processed, and a random pattern will take the audio and control the LEDs based off of it in various ways.
For example, one effect might flash all LEDs white, with the intensity of the white computed from the RMS amplitude of the audio.

# Hardware details

Both the transmitter and the receiver are based on the Raspberry Pi Pico 2 W.

## Transmitter

Mono audio is sampled from GPIO pin 26 and needs to be DC biased to the center of the ADC range.
A potentiometer is useful for adjusting the gain of the signal.
The left and right channels may be combined.
This can be accomplished as follows:

* Connect two 10 µF ceramic capacitors to a 3.5 mm audio jack, one per channel
* Connect the other ends of the capacitor together
* Conenct the capacitors ends to the wiper of a potentiometer
* Connect either end of the potentiometer to a voltage divider 
* Form the voltage divider by connecting two 600 Ohm resistors in series between 3.3v and GND

The combined left and right channels have an impedance of 300 Ohm,
which is matched by the voltage divider at higher frequencies.
The potentiometer will throw off this matching, but it's probably fine for this application.
Constructing a wideband fixed-impedance variable attenuator is left as an exercise for the reader.

## Receiver

The receiver has one or more WS2812 strings with their data pins connected to GPIO pin 2 and up.
The same light data are sent to all strings.

External +5V
Diode from +5V USB

# Software details

The audio is broadcast over UDP port 4445.
It is nominally 48 kHz, 16 bits per sample, mono.
Sometimes the sampling fails to meet the realtime deadline, so the actual sampling rate can be somewhat lower.
Fortunately this doesn't matter.
Each UDP packet is 960 bytes, containing 480 samples of audio, or 10 ms.
This is about as large as these packets can be without causing fragmentation,
which kills the WiFi performance.
