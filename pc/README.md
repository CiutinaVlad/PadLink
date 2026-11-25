# PadLink PC receiver (Wi-Fi mode)

Only needed for **PC – Wi-Fi** mode. The USB modes need nothing on the PC.

```
pip install vgamepad        # Windows: also install the ViGEmBus driver
pip install evdev           # Linux
python padlink_receiver.py
```
On the Switch, pick **PC – Wi-Fi**. If it can't find the PC, put the PC's IP address in `switch/padlink/pc_ip.txt` on the SD card.

`python fake_switch.py` tests the receiver without a Switch.

## Wii U in Cemu

```
python padlink_receiver.py --cemu
pip install pynput          # macOS / Linux only, for touch
```
On the Switch pick **Wii U (Cemu)**. In Cemu's input settings choose **DSUController**, `127.0.0.1`, port `26760`.
Touch goes to Cemu's **GamePad View** window (Options > Separate GamePad view); use `--touch-rect x,y,w,h` to point it somewhere else.
`python fake_switch.py 127.0.0.1 10 --cemu` tests it without a Switch.
