"""Config flow for ATOMCam integration."""

from __future__ import annotations

import asyncio
import logging

import aiohttp
import voluptuous as vol

from homeassistant.config_entries import ConfigFlow
from homeassistant.data_entry_flow import FlowResult

from .const import CONF_BASE_URL, CONF_HOST, DEFAULT_NAME, DOMAIN

_LOGGER = logging.getLogger(__name__)


async def _test_connection(host: str, base_url: str) -> bool:
    """Test connection to the camera by sending astream status query."""
    if base_url:
        url = f"{base_url.rstrip('/')}/cgi-bin/cmd.cgi?port=socket"
    else:
        url = f"http://{host}/cgi-bin/cmd.cgi?port=socket"

    session = aiohttp.ClientSession()
    try:
        async with session.post(
            url,
            json={"exec": "astream"},
            timeout=aiohttp.ClientTimeout(total=5),
        ) as resp:
            return resp.status == 200
    except (aiohttp.ClientError, asyncio.TimeoutError):
        return False
    finally:
        await session.close()


class AtomCamConfigFlow(ConfigFlow, domain=DOMAIN):
    """Handle a config flow for ATOMCam."""

    VERSION = 1

    async def async_step_user(
        self, user_input: dict | None = None
    ) -> FlowResult:
        """Handle the initial step."""
        errors = {}

        if user_input is not None:
            host = user_input[CONF_HOST]
            base_url = user_input.get(CONF_BASE_URL, "")

            if await _test_connection(host, base_url):
                await self.async_set_unique_id(f"atomcam_{host.replace('.', '_')}")
                self._abort_if_unique_id_configured()

                return self.async_create_entry(
                    title=DEFAULT_NAME,
                    data={
                        CONF_HOST: host,
                        CONF_BASE_URL: base_url,
                    },
                )
            errors["base"] = "cannot_connect"

        return self.async_show_form(
            step_id="user",
            data_schema=vol.Schema(
                {
                    vol.Required(CONF_HOST): str,
                    vol.Optional(CONF_BASE_URL, default=""): str,
                }
            ),
            errors=errors,
        )
