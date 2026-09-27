import argparse
import json
import os
import sys
from urllib.error import HTTPError
from urllib.parse import urlencode
from urllib.request import urlopen


def get_json(url):
    with urlopen(url, timeout=5) as response:
        return response.status, json.loads(response.read())


def check_number(value, name):
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        raise AssertionError(f"{name} must be a number, got {value!r}")


def run_checks(base_url):
    base_url = base_url.rstrip("/")

    status_code, config = get_json(f"{base_url}/config")
    assert status_code == 200, f"GET /config returned HTTP {status_code}"
    check_number(config.get("temp_on"), "temp_on")
    check_number(config.get("temp_off"), "temp_off")
    assert config["temp_off"] < config["temp_on"], "Stored thresholds are invalid"
    print("PASS GET /config returns valid thresholds")

    status_code, status = get_json(f"{base_url}/status")
    assert status_code == 200, f"GET /status returned HTTP {status_code}"
    for key in ("abluft_temp", "abluft_humidity"):
        value = status.get(key)
        if value is not None:
            check_number(value, key)
    assert isinstance(status.get("fan_state"), bool), "fan_state must be boolean"
    check_number(status.get("temp_on"), "status.temp_on")
    check_number(status.get("temp_off"), "status.temp_off")
    print("PASS GET /status returns sensor and fan state")

    invalid_url = f"{base_url}/config?{urlencode({'temp_off': config['temp_on']})}"
    try:
        get_json(invalid_url)
    except HTTPError as error:
        assert error.code == 400, f"Invalid config returned HTTP {error.code}, expected 400"
    else:
        raise AssertionError("Invalid thresholds were not rejected with HTTP 400")

    _, unchanged_config = get_json(f"{base_url}/config")
    assert unchanged_config["temp_on"] == config["temp_on"], "Rejected request changed temp_on"
    assert unchanged_config["temp_off"] == config["temp_off"], "Rejected request changed temp_off"
    print("PASS invalid thresholds are rejected without changing active config")


def main():
    parser = argparse.ArgumentParser(description="Smoke-test the KS Abluft ESP32 HTTP API")
    parser.add_argument(
        "--url",
        default=os.environ.get("DEVICE_URL"),
        help="Base URL of the ESP32, e.g. http://192.168.1.42 (or set DEVICE_URL)",
    )
    args = parser.parse_args()
    if not args.url:
        parser.error("provide --url or set DEVICE_URL")

    try:
        run_checks(args.url)
    except Exception as error:
        print(f"FAIL {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())