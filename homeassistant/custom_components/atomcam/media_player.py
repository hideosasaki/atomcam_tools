"""ATOMCam Speaker media player platform."""

from __future__ import annotations

import asyncio
import logging
import os
import shutil
import tempfile

import aiohttp

from homeassistant.components.media_player import (
    MediaPlayerDeviceClass,
    MediaPlayerEntity,
    MediaPlayerEntityFeature,
    MediaPlayerState,
    MediaType,
)
from homeassistant.config_entries import ConfigEntry
from homeassistant.core import HomeAssistant
from homeassistant.helpers.entity_platform import AddEntitiesCallback

from .const import CONF_BASE_URL, CONF_HOST, DEFAULT_VOLUME, DOMAIN

_LOGGER = logging.getLogger(__name__)


async def async_setup_entry(
    hass: HomeAssistant, entry: ConfigEntry, async_add_entities: AddEntitiesCallback
) -> None:
    """Set up ATOMCam media player from a config entry."""
    data = hass.data[DOMAIN][entry.entry_id]
    host = data[CONF_HOST]
    base_url = data.get(CONF_BASE_URL, "")
    name = entry.title
    async_add_entities([AtomCamMediaPlayer(hass, entry.entry_id, name, host, base_url)])


class AtomCamMediaPlayer(MediaPlayerEntity):
    """ATOMCam Speaker as a Home Assistant media player."""

    _attr_device_class = MediaPlayerDeviceClass.SPEAKER
    _attr_supported_features = (
        MediaPlayerEntityFeature.PLAY_MEDIA
        | MediaPlayerEntityFeature.VOLUME_SET
        | MediaPlayerEntityFeature.VOLUME_STEP
        | MediaPlayerEntityFeature.STOP
    )
    _attr_media_content_type = MediaType.MUSIC

    def __init__(
        self,
        hass: HomeAssistant,
        entry_id: str,
        name: str,
        host: str,
        base_url: str,
    ) -> None:
        """Initialize the ATOMCam media player."""
        self._hass = hass
        self._entry_id = entry_id
        self._attr_name = name
        self._attr_unique_id = f"atomcam_speaker_{host.replace('.', '_')}"
        self._host = host
        self._base_url = base_url.rstrip("/") if base_url else ""
        self._attr_state = MediaPlayerState.IDLE
        self._attr_volume_level = DEFAULT_VOLUME / 100.0
        self._playing = False

    @property
    def _cmd_url(self) -> str:
        """URL for cmd.cgi."""
        if self._base_url:
            return f"{self._base_url}/cgi-bin/cmd.cgi?port=socket"
        return f"http://{self._host}/cgi-bin/cmd.cgi?port=socket"

    @property
    def _stream_url(self) -> str:
        """URL for stream.cgi."""
        if self._base_url:
            return f"{self._base_url}/cgi-bin/stream.cgi"
        return f"http://{self._host}/cgi-bin/stream.cgi"

    async def _send_cmd(self, command: str) -> str:
        """Send a command to the camera via cmd.cgi."""
        session = aiohttp.ClientSession()
        try:
            async with session.post(
                self._cmd_url,
                json={"exec": command},
                timeout=aiohttp.ClientTimeout(total=10),
            ) as resp:
                return await resp.text()
        except (aiohttp.ClientError, asyncio.TimeoutError) as err:
            _LOGGER.error("Failed to send command '%s': %s", command, err)
            return "error"
        finally:
            await session.close()

    async def _post_pcm(self, pcm_data: bytes) -> bool:
        """POST raw PCM data to stream.cgi with Content-Length."""
        session = aiohttp.ClientSession()
        try:
            async with session.post(
                self._stream_url,
                data=pcm_data,
                headers={
                    "Content-Type": "application/octet-stream",
                    "Content-Length": str(len(pcm_data)),
                },
                timeout=aiohttp.ClientTimeout(total=60),
            ) as resp:
                text = await resp.text()
                if resp.status != 200 or "ok" not in text:
                    _LOGGER.error("stream.cgi error: HTTP %s, body=%s", resp.status, text)
                    return False
                return True
        except (aiohttp.ClientError, asyncio.TimeoutError) as err:
            _LOGGER.error("Failed to POST PCM to stream.cgi: %s", err)
            return False
        finally:
            await session.close()

    async def _convert_to_pcm(self, input_path: str, output_path: str) -> bool:
        """Convert audio file to raw PCM (s16le, 8kHz, mono) using ffmpeg."""
        proc = await asyncio.create_subprocess_exec(
            "ffmpeg", "-i", input_path,
            "-f", "s16le", "-ar", "8000", "-ac", "1",
            output_path, "-y",
            stdout=asyncio.subprocess.DEVNULL,
            stderr=asyncio.subprocess.PIPE,
        )
        _, stderr = await proc.communicate()
        if proc.returncode != 0:
            _LOGGER.error("ffmpeg failed (exit %s): %s", proc.returncode, stderr.decode())
            return False
        return True

    async def _resolve_media_url(self, media_id: str) -> str:
        """Resolve media-source:// URI to an HTTP URL."""
        if media_id.startswith("media-source://"):
            from homeassistant.components.media_source import async_resolve_media
            result = await async_resolve_media(self._hass, media_id, None)
            return result.url
        return media_id

    async def _download_media(self, url: str, dest_path: str) -> bool:
        """Download media from a URL to a local file."""
        if url.startswith("/"):
            internal_url = self._hass.config.internal_url or "http://localhost:8123"
            url = f"{internal_url}{url}"

        session = aiohttp.ClientSession()
        try:
            async with session.get(
                url, timeout=aiohttp.ClientTimeout(total=30)
            ) as resp:
                if resp.status != 200:
                    _LOGGER.error("Failed to download media: HTTP %s", resp.status)
                    return False
                data = await resp.read()
                await self._hass.async_add_executor_job(
                    self._write_file, dest_path, data
                )
                return True
        except (aiohttp.ClientError, asyncio.TimeoutError) as err:
            _LOGGER.error("Failed to download media: %s", err)
            return False
        finally:
            await session.close()

    @staticmethod
    def _write_file(path: str, data: bytes) -> None:
        """Write bytes to file (runs in executor)."""
        with open(path, "wb") as f:
            f.write(data)

    @staticmethod
    def _read_file(path: str) -> bytes:
        """Read bytes from file (runs in executor)."""
        with open(path, "rb") as f:
            return f.read()

    @staticmethod
    def _make_tmpdir() -> str:
        """Create temp directory (runs in executor)."""
        return tempfile.mkdtemp()

    @staticmethod
    def _remove_tmpdir(path: str) -> None:
        """Remove temp directory (runs in executor)."""
        shutil.rmtree(path, ignore_errors=True)

    async def async_play_media(
        self, media_type: MediaType, media_id: str, **kwargs
    ) -> None:
        """Play media on the ATOMCam speaker."""
        _LOGGER.info("play_media: type=%s, id=%s", media_type, media_id)

        media_id = await self._resolve_media_url(media_id)
        _LOGGER.debug("Resolved media URL: %s", media_id)

        tmpdir = await self._hass.async_add_executor_job(self._make_tmpdir)
        try:
            input_file = os.path.join(tmpdir, "input")
            pcm_file = os.path.join(tmpdir, "output.pcm")

            _LOGGER.debug("Downloading media from %s", media_id)
            if not await self._download_media(media_id, input_file):
                return

            _LOGGER.debug("Converting to PCM")
            if not await self._convert_to_pcm(input_file, pcm_file):
                return

            pcm_data = await self._hass.async_add_executor_job(self._read_file, pcm_file)
            volume = int(self._attr_volume_level * 100)

            self._playing = True
            self._attr_state = MediaPlayerState.PLAYING
            self.async_write_ha_state()

            # Start astream on camera via cmd.cgi (non-blocking, it blocks until FIFO ends)
            _LOGGER.debug("Starting astream (volume=%s)", volume)
            astream_task = asyncio.create_task(
                self._send_cmd(f"astream /tmp/audio_in.fifo {volume}")
            )

            # Small delay to let astream open the FIFO reader
            await asyncio.sleep(0.5)

            # POST PCM data to stream.cgi
            _LOGGER.debug("POSTing %d bytes of PCM to stream.cgi", len(pcm_data))
            await self._post_pcm(pcm_data)

            # Wait for astream to finish
            try:
                await asyncio.wait_for(astream_task, timeout=10)
            except asyncio.TimeoutError:
                _LOGGER.warning("astream did not finish in time")
                astream_task.cancel()

            self._playing = False
            self._attr_state = MediaPlayerState.IDLE
            self.async_write_ha_state()
            _LOGGER.debug("Playback complete")
        finally:
            await self._hass.async_add_executor_job(self._remove_tmpdir, tmpdir)

    async def async_set_volume_level(self, volume: float) -> None:
        """Set volume level (0.0 to 1.0)."""
        self._attr_volume_level = volume
        self.async_write_ha_state()

    async def async_media_stop(self) -> None:
        """Stop current playback."""
        await self._send_cmd("astream stop")
        self._playing = False
        self._attr_state = MediaPlayerState.IDLE
        self.async_write_ha_state()

    @property
    def available(self) -> bool:
        """Return True if the entity is available."""
        return True
