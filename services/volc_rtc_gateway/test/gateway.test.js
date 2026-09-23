const assert = require("node:assert/strict");
const path = require("node:path");
const test = require("node:test");
const {
  OPUS_ROOM_PREFIX,
  SessionManager,
  START_ACTION,
  STOP_ACTION,
} = require("../src/session_manager");
const {
  loadVoiceChatConfig,
  rejectForbiddenVoiceChatFields,
} = require("../src/config");
const { createHttpServer } = require("../src/server");
const { createSignedRequest } = require("../src/volc_openapi");

const config = {
  appId: "012345678901234567890123",
  appKey: "01234567890123456789012345678901",
  accessKeyId: "ak",
  secretAccessKey: "sk",
  deviceApiKey: "012345678901234567890123",
  sessionTtlSeconds: 3600,
  voiceChat: {
    AgentConfig: { UserId: "espclaw_voice_agent" },
    Config: {
      ASRConfig: { Provider: "volcano" },
      LLMConfig: { Mode: "ArkV3" },
      TTSConfig: { Provider: "volcano_bidirection" },
    },
  },
};

function successfulInvoker(calls) {
  return async (action, body) => {
    calls.push({ action, body });
    return { ResponseMetadata: { Action: action }, Result: "ok" };
  };
}

test("session manager creates short-lived credentials and starts VoiceChat", async () => {
  const calls = [];
  const manager = new SessionManager(config, successfulInvoker(calls));
  const credentials = await manager.start("espclaw-aabbccddeeff");

  assert.equal(calls.length, 1);
  assert.equal(calls[0].action, START_ACTION);
  assert.equal(calls[0].body.AppId, config.appId);
  assert.equal(calls[0].body.RoomId, credentials.room_id);
  assert.match(credentials.room_id, new RegExp(`^${OPUS_ROOM_PREFIX}[0-9a-f]{32}$`));
  assert.deepEqual(calls[0].body.AgentConfig.TargetUserId, [credentials.user_id]);
  assert.match(calls[0].body.AgentConfig.UserId, /^bot_[0-9a-f]{32}$/);
  assert.notEqual(calls[0].body.AgentConfig.UserId, config.voiceChat.AgentConfig.UserId);
  assert.equal(calls[0].body.AgentConfig.EnableConversationStateCallback, true);
  assert.deepEqual(calls[0].body.AgentConfig.Burst, {
    Enable: false,
    BufferSize: 500,
    Interval: 20,
  });
  assert.match(credentials.token, new RegExp(`^001${config.appId}`));
  assert.equal(credentials.token.includes(config.appKey), false);
  assert.ok(credentials.expires_at > Math.floor(Date.now() / 1000));
});

test("starting a second session for a device stops the first task", async () => {
  const calls = [];
  const manager = new SessionManager(config, successfulInvoker(calls));
  await manager.start("espclaw-aabbccddeeff");
  await manager.start("espclaw-aabbccddeeff");
  assert.deepEqual(
    calls.map(({ action }) => action),
    [START_ACTION, STOP_ACTION, START_ACTION]
  );
});

test("session manager rejects a cloud response without Result=ok", async () => {
  const manager = new SessionManager(
    config,
    async (action) => ({ ResponseMetadata: { Action: action } }),
    { info() {}, warn() {} }
  );
  await assert.rejects(
    manager.start("espclaw-missing-result"),
    /StartVoiceChat failed: MISSING_RESULT/
  );
});

test("concurrent starts for one device are serialized", async () => {
  const calls = [];
  const manager = new SessionManager(config, async (action, body) => {
    calls.push({ action, body });
    await new Promise((resolve) => setTimeout(resolve, 5));
    return { ResponseMetadata: { Action: action }, Result: "ok" };
  });

  const [first, second] = await Promise.all([
    manager.start("espclaw-concurrent"),
    manager.start("espclaw-concurrent"),
  ]);

  assert.notEqual(first.session_id, second.session_id);
  assert.deepEqual(
    calls.map(({ action }) => action),
    [START_ACTION, STOP_ACTION, START_ACTION]
  );
});

test("OpenAPI request uses the documented HMAC-SHA256 credential scope", () => {
  const request = createSignedRequest(
    config,
    START_ACTION,
    { AppId: config.appId, RoomId: "room", TaskId: "task" },
    new Date("2026-09-19T08:07:06.000Z")
  );
  assert.equal(
    request.url,
    "https://rtc.volcengineapi.com/?Action=StartVoiceChat&Version=2024-12-01"
  );
  assert.equal(request.options.headers["X-Date"], "20260919T080706Z");
  assert.match(
    request.options.headers.Authorization,
    /^HMAC-SHA256 Credential=ak\/20260919\/cn-north-1\/rtc\/request, /
  );
  assert.equal(
    request.options.headers.Authorization,
    "HMAC-SHA256 Credential=ak/20260919/cn-north-1/rtc/request, " +
      "SignedHeaders=content-type;host;x-content-sha256;x-date, " +
      "Signature=ae41da0dbf107994692ddab9b208bac20049f3d451e8f4f8863d54c4d11fec82"
  );
});

test("VoiceChat JSON only permits documented runtime service credentials", () => {
  assert.throws(
    () => rejectForbiddenVoiceChatFields({ Config: { Token: "secret" } }),
    /must not set voice_chat\.Config\.Token/
  );
  assert.throws(
    () => rejectForbiddenVoiceChatFields({ AppId: "dynamic" }),
    /must not set voice_chat\.AppId/
  );
  assert.throws(
    () => rejectForbiddenVoiceChatFields({ Config: { AppId: "misplaced" } }),
    /must not set voice_chat\.Config\.AppId/
  );
  assert.doesNotThrow(() =>
    rejectForbiddenVoiceChatFields({
      Config: {
        ASRConfig: {
          ProviderParams: {
            AppId: "asr-app-id",
            AccessToken: "asr-access-token",
            ApiResourceId: "volc.example",
          },
        },
        TTSConfig: {
          ProviderParams: {
            app: { appid: "tts-app-id", token: "tts-access-token" },
          },
        },
      },
    })
  );
});

test("VoiceChat config injects every official runtime service credential", () => {
  const voiceChat = loadVoiceChatConfig(
    path.resolve(__dirname, "../config/voice_chat.json"),
    {
      ASR_APP_ID: "asr-app-id",
      ASR_ACCESS_TOKEN: "asr-access-token",
      TTS_APP_ID: "tts-app-id",
      TTS_ACCESS_TOKEN: "tts-access-token",
      ARK_ENDPOINT_ID: "ep-example",
    }
  );

  assert.deepEqual(voiceChat.Config.ASRConfig.ProviderParams, {
    Mode: "bigmodel",
    AppId: "asr-app-id",
    AccessToken: "asr-access-token",
    ApiResourceId: "volc.bigasr.sauc.duration",
    StreamMode: 0,
  });
  assert.deepEqual(voiceChat.Config.TTSConfig.ProviderParams.app, {
    appid: "tts-app-id",
    token: "tts-access-token",
  });
  assert.equal(
    voiceChat.Config.TTSConfig.ProviderParams.ResourceId,
    "volc.service_type.10029"
  );
  assert.equal(
    voiceChat.Config.TTSConfig.ProviderParams.audio.voice_type,
    "zh_female_xiaohe_jupiter_bigtts"
  );
  assert.equal(voiceChat.Config.LLMConfig.EndPointId, "ep-example");
  assert.equal(Object.hasOwn(voiceChat.AgentConfig, "UserId"), false);
  assert.equal(Object.hasOwn(voiceChat.Config.LLMConfig, "ModelName"), false);
  assert.equal(voiceChat.Config.ASRConfig.VolumeGain, 0.3);
  assert.equal(
    Object.hasOwn(voiceChat.Config.ASRConfig.VADConfig, "VolumeGain"),
    false
  );
});

test("HTTP API requires the device key and never returns long-lived secrets", async (t) => {
  const calls = [];
  const manager = new SessionManager(config, successfulInvoker(calls));
  const server = createHttpServer(config, manager, {
    error() {},
    warn() {},
  });
  server.listen(0, "127.0.0.1");
  await new Promise((resolve) => server.once("listening", resolve));
  t.after(async () => {
    await manager.close();
    await new Promise((resolve) => server.close(resolve));
  });
  const origin = `http://127.0.0.1:${server.address().port}`;

  assert.equal((await fetch(`${origin}/health`)).status, 200);
  assert.equal(
    (
      await fetch(`${origin}/v1/rtc/sessions`, {
        method: "POST",
        headers: { "content-type": "application/json" },
        body: JSON.stringify({ device_id: "espclaw-aabbccddeeff" }),
      })
    ).status,
    401
  );

  const response = await fetch(`${origin}/v1/rtc/sessions`, {
    method: "POST",
    headers: {
      authorization: `Bearer ${config.deviceApiKey}`,
      "content-type": "application/json",
    },
    body: JSON.stringify({ device_id: "espclaw-aabbccddeeff" }),
  });
  assert.equal(response.status, 201);
  const body = await response.json();
  assert.equal(body.app_id, config.appId);
  assert.equal(JSON.stringify(body).includes(config.appKey), false);
  assert.equal(JSON.stringify(body).includes(config.secretAccessKey), false);
});
