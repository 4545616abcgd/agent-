const crypto = require("node:crypto");
const http = require("node:http");
const path = require("node:path");
const { loadEnvFile, loadRuntimeConfig } = require("./config");
const { CloudApiError, SessionManager } = require("./session_manager");
const { RTC_API_VERSION, createOpenApiInvoker } = require("./volc_openapi");

const MAX_BODY_BYTES = 4096;

function sendJson(response, statusCode, value) {
  const body = Buffer.from(JSON.stringify(value));
  response.writeHead(statusCode, {
    "Content-Type": "application/json; charset=utf-8",
    "Content-Length": body.length,
    "Cache-Control": "no-store",
    "X-Content-Type-Options": "nosniff",
  });
  response.end(body);
}

function bearerMatches(request, expected) {
  const header = request.headers.authorization || "";
  const actual = header.startsWith("Bearer ") ? header.slice(7) : "";
  const actualBuffer = Buffer.from(actual);
  const expectedBuffer = Buffer.from(expected);
  return (
    actualBuffer.length === expectedBuffer.length &&
    crypto.timingSafeEqual(actualBuffer, expectedBuffer)
  );
}

async function readJson(request) {
  const chunks = [];
  let total = 0;
  for await (const chunk of request) {
    total += chunk.length;
    if (total > MAX_BODY_BYTES) {
      const error = new Error("request body is too large");
      error.statusCode = 413;
      throw error;
    }
    chunks.push(chunk);
  }
  if (total === 0) {
    return {};
  }
  try {
    return JSON.parse(Buffer.concat(chunks).toString("utf8"));
  } catch {
    const error = new Error("request body must be valid JSON");
    error.statusCode = 400;
    throw error;
  }
}

function createHttpServer(config, manager, logger = console) {
  return http.createServer(async (request, response) => {
    const url = new URL(request.url, "http://localhost");
    if (request.method === "GET" && url.pathname === "/health") {
      sendJson(response, 200, {
        ok: true,
        service: "esp-claw-volc-rtc-gateway",
        rtc_api_version: RTC_API_VERSION,
      });
      return;
    }
    if (!bearerMatches(request, config.deviceApiKey)) {
      sendJson(response, 401, { error: "unauthorized" });
      return;
    }

    try {
      if (request.method === "POST" && url.pathname === "/v1/rtc/sessions") {
        const body = await readJson(request);
        const credentials = await manager.start(body.device_id);
        sendJson(response, 201, credentials);
        return;
      }

      const stopMatch = url.pathname.match(
        /^\/v1\/rtc\/sessions\/([0-9a-f-]+)\/stop$/i
      );
      if (request.method === "POST" && stopMatch) {
        const stopped = await manager.stop(stopMatch[1]);
        sendJson(response, 200, { stopped });
        return;
      }

      sendJson(response, 404, { error: "not_found" });
    } catch (error) {
      if (error instanceof CloudApiError) {
        logger.error("RTC cloud API rejected the request", {
          action: error.action,
          code: error.cloudCode,
          requestId: error.requestId,
        });
        sendJson(response, 502, {
          error: "rtc_cloud_error",
          code: error.cloudCode,
          request_id: error.requestId,
        });
        return;
      }
      const statusCode = error.statusCode || (error instanceof TypeError ? 400 : 500);
      logger.error("RTC gateway request failed", { error: error.message });
      sendJson(response, statusCode, {
        error: statusCode === 500 ? "internal_error" : error.message,
      });
    }
  });
}

async function startServer() {
  const serviceRoot = path.resolve(__dirname, "..");
  loadEnvFile(path.join(serviceRoot, ".env.local"));
  const config = loadRuntimeConfig(process.env, serviceRoot);
  const manager = new SessionManager(config, createOpenApiInvoker(config));
  const server = createHttpServer(config, manager);
  const reapTimer = setInterval(() => {
    void manager.reapExpired();
  }, 60000);
  reapTimer.unref();

  server.listen(config.port, config.host, () => {
    console.log(
      `ESP-Claw RTC gateway listening on http://${config.host}:${config.port}`
    );
  });

  let stopping = false;
  const stop = async () => {
    if (stopping) return;
    stopping = true;
    clearInterval(reapTimer);
    await manager.close();
    await new Promise((resolve) => server.close(resolve));
  };
  for (const signal of ["SIGINT", "SIGTERM"]) {
    process.once(signal, () => {
      void stop().then(() => process.exit(0));
    });
  }
  return { server, manager, stop };
}

if (require.main === module) {
  void startServer().catch((error) => {
    console.error(`RTC gateway failed to start: ${error.message}`);
    process.exit(1);
  });
}

module.exports = {
  createHttpServer,
  readJson,
  startServer,
};
