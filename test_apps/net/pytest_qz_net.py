# Owner-run: `pytest --target esp32s3` from this directory with the Watchy v3 attached.
import pytest
from pytest_embedded import Dut


@pytest.mark.esp32s3
def test_qz_net(dut: Dut) -> None:
    dut.run_all_single_board_cases()
