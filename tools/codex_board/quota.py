"""Read quota through Codex App Server using the existing login, over stdio."""
import asyncio
import json


async def read_limits(command="codex", timeout=25):
    proc = await asyncio.create_subprocess_exec(
        command, "app-server", "--listen", "stdio://",
        stdin=asyncio.subprocess.PIPE, stdout=asyncio.subprocess.PIPE,
        stderr=asyncio.subprocess.DEVNULL, limit=1024 * 1024,
    )

    async def send(value):
        proc.stdin.write(json.dumps(value).encode() + b"\n")
        await proc.stdin.drain()

    async def response(request_id):
        while True:
            line = await proc.stdout.readline()
            if not line:
                raise RuntimeError("App Server closed the connection")
            message = json.loads(line)
            if message.get("id") != request_id:
                continue
            if "error" in message:
                # Do not forward server errors or account details to the feed.
                raise RuntimeError("Quota request was unavailable")
            return message["result"]

    async def exchange():
        await send({"id": 1, "method": "initialize", "params": {
            "clientInfo": {"name": "codex_board", "title": "Codex Board", "version": "0.1.0"}}})
        await response(1)
        await send({"method": "initialized", "params": {}})
        await send({"id": 2, "method": "account/rateLimits/read"})
        return await response(2)

    try:
        return await asyncio.wait_for(exchange(), timeout)
    finally:
        if proc.returncode is None:
            proc.terminate()
            try:
                await asyncio.wait_for(proc.wait(), 3)
            except asyncio.TimeoutError:
                proc.kill()
                await proc.wait()
