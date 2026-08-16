"""Expose the public Bluedroid headers to the Arduino BLE compatibility layer."""

from pathlib import Path

Import("env")

idf_dir = Path(env.PioPlatform().get_package_dir("framework-espidf"))
api_dir = idf_dir / "components" / "bt" / "host" / "bluedroid" / "api" / "include" / "api"

if not (api_dir / "esp_gatt_defs.h").is_file():
    raise RuntimeError(f"Bluedroid public headers not found: {api_dir}")

env.Append(CPPPATH=[str(api_dir)])
