"""Test-schema registry. Maps a test name to its CSV-to-Flent layout."""
from __future__ import annotations

from .host_isolation import HOST_ISOLATION_SCHEMA
from .rrul import RRUL_SCHEMA
from .tcp_4up_squarewave import TCP_4UP_SQUAREWAVE_SCHEMA
from .tcp_download import TCP_DOWNLOAD_SCHEMA
from .tcp_upload import TCP_UPLOAD_SCHEMA

SCHEMAS: dict = {
    "host_isolation": HOST_ISOLATION_SCHEMA,
    "rrul": RRUL_SCHEMA,
    "tcp_4up_squarewave": TCP_4UP_SQUAREWAVE_SCHEMA,
    "tcp_download": TCP_DOWNLOAD_SCHEMA,
    "tcp_upload": TCP_UPLOAD_SCHEMA,
}
