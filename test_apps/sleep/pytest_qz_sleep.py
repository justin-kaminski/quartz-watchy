# Owner-run on the Watchy v3: `pytest --target esp32s3` from this directory, or flash and drive the
# menu by hand (see the prompts printed by each case). The multi-stage cases need the owner to press
# a button when prompted; the console drops while the chip is in deep sleep (USB-Serial-JTAG).
import pytest
from pytest_embedded import Dut


@pytest.mark.esp32s3
def test_qz_sleep(dut: Dut) -> None:
    dut.run_all_single_board_cases()
