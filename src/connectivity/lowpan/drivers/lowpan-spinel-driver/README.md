OpenThread/Spinel LoWPAN Driver
===============================

This LoWPAN driver takes a `fuchsia.lowpan.spinel::Device` and provides
a `fuchsia.lowpan::Device`, which it registers with the LoWPAN Service.

### Connectivity State Diagram

![LoWPAN Connectivity State Diagram](doc/lowpan-connectivity-state.svg)

### Init State Diagram

![LoWPAN Spinel Driver Init State Diagram](doc/lowpan-init-state.svg)
