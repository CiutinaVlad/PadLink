# PadLink PC receiver (Wi-Fi mode)

Only needed for **PC – Wi-Fi** mode. The USB modes need nothing on the PC.

```
pip install vgamepad        # Windows: also install the ViGEmBus driver
pip install evdev           # Linux
python padlink_receiver.py
```
On the Switch, pick **PC – Wi-Fi**. If it can't find the PC, put the PC's IP address in `switch/padlink/pc_ip.txt` on the SD card.

`python fake_switch.py` tests the receiver without a Switch.
