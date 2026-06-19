# TODO

Running list of follow-ups that aren't urgent enough to block other work.

## Safety

- [x] **Test Fire button accidental-touch protection.** ~~The red `#testFireBtn` fires the solenoid on a single touch with no confirmation, so scrolling on a phone can trigger a real fire.~~ Resolved: the Test Fire tab now has an **arm-cover toggle** that must be ON before the fire button reacts. The cover re-closes on every page load and on device reboot (gated by `boot_id`). The button also lives at the top of its own tab, out of the general scroll path. See the Test Fire panel in `tools/web-preview/index.html` / `Test_Button_DMX/web.cpp`.
