# Comparator board

English | [Русский](README.ru.md)

Pulse input board for eMeter32. It converts the pulse outputs of the electricity meters into clean logic-level signals for the ESP32. The board is built around the **LM393** dual comparator: one comparator per energy channel (**E1** and **E2**).

## How it works

An electricity meter signals consumed energy with pulses (for example, an S0 output or an optical LED). The comparator turns these pulses into a sharp digital signal, and the ESP32 counts them on the hardware PCNT peripheral. Energy is calculated as `kWh = pulses / constant`, where the constant (pulses per kWh) is set in the device settings (3200 by default).

## Schematic

[![Comparator board schematic](schematic/Schematic_Comparator_2026-10-16.png)](schematic/Schematic_Comparator_2026-10-16.pdf)

Click the image to open the schematic as a PDF.

## Connection to the ESP32

The board outputs go to the pins defined in [`board_config.h`](../../components/board_config/include/board_config.h):

| Signal | ESP32 pin |
|---|---|
| Channel E1 output | GPIO 32 |
| Channel E2 output | GPIO 33 |

The LM393 has open-collector outputs, so the firmware enables the internal pull-up on both inputs (`PULSE_PULL_UP`). The counter uses a hardware glitch filter of 10 µs (`PULSE_GLITCH_NS`) and counts the rising edge by default (`PULSE_COUNT_FALLING`). See the [`pulse_counter`](../../components/pulse_counter) component for details.

## Files

| File | Description |
|---|---|
| [`schematic/Schematic_Comparator_2026-10-16.pdf`](schematic/Schematic_Comparator_2026-10-16.pdf) | Circuit diagram (PDF), dated 2026-10-16 |
| [`schematic/Schematic_Comparator_2026-10-16.png`](schematic/Schematic_Comparator_2026-10-16.png) | Circuit diagram (image for the README) |
| [`pcb/Gerber_Comparator_PCB_Comparator_2026-10-04.zip`](pcb/Gerber_Comparator_PCB_Comparator_2026-10-04.zip) | Gerber files for PCB manufacturing, dated 2026-10-04 |

Component values are shown on the schematic. The PCB project sources are not included in the repository.

## Ordering and building the PCB

1. Upload the Gerber archive from [`pcb/`](pcb) to a PCB manufacturer without unpacking it (most services accept a zip).
2. Order the components shown on the [schematic](schematic/Schematic_Comparator_2026-10-16.pdf).
3. Solder the board and check the outputs with a multimeter or an oscilloscope before connecting it to the ESP32.

TODO: PCB size, number of layers and thickness.

## Setting up the counting

- The number of pulses per kWh is printed on your meter (for example, 3200 imp/kWh). Enter it as the **Meter constant** in the setup portal or in Home Assistant.
- If your meter needs a different counting edge, change `PULSE_COUNT_FALLING` in `board_config.h`.
- Initial readings of the counters can be set in the setup portal or with the **Set energy E1/E2** entities in Home Assistant.

## Notes

- Check the polarity and the voltage levels of your meters' pulse outputs before connecting them: the inputs must not exceed the ratings of the comparator.
- The board may be connected to circuits close to **230 V**. Keep it insulated and build it only if you know electrical safety rules.

## License

The design files are released under the same [MIT License](../../LICENSE) as the project.
