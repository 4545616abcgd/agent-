const fs = require("node:fs");
const path = require("node:path");

function loadEnvFile(filePath, env = process.env) {
  if (!fs.existsSync(filePath)) {
    return env;
  }
  for (const line of fs.readFileSync(filePath, "utf8").split(/\r?\n/)) {
    const match = line.match(/^\s*([A-Za-z_][A-Za-z0-9_]*)\s*=\s*(.*?)\s*$/);
    if (!match || Object.hasOwn(env, match[1])) {
      continue;
    }
    let value = match[2];
    if (
      value.length >= 2 &&
      ((value.startsWith('"') && value.endsWith('"')) ||
        (value.startsWith("'") && value.endsWith("'")))
    ) {
      value = value.slice(1, -1);
    }
    env[match[1]] = value;
  }
  return env;
}

function required(value, name) {
  const normalized = value?.trim();
  if (!normalized) {
    throw new Error(`${name} is required`);
  }
  return normalized;
}

function parseInteger(value, name, fallback, min, max) {
  const parsed = Number(value ?? fallback);
  if (!Number.isInteger(parsed) || parsed < min || parsed > max) {
    throw new Error(`${name} must be an integer between ${min} and ${max}`);
  }
  return parsed;
}

function expandEnvReferences(value, env) {
  if (Array.isArray(value)) {
    return value.map((item) => expandEnvReferences(item, env));
  }
  if (value && typeof value === "object") {
    return Object.fromEntries(
      Object.entries(value).map(([key, item]) => [
        key,
        expandEnvReferences(item, env),
      ])
    );
  }
  if (typeof value !== "string") {
    return value;
  }
  const match = value.match(/^\$\{([A-Z_][A-Z0-9_]*)\}$/);
  if (!match) {
    return value;
  }
  return required(env[match[1]], match[1]);
}

const FORBIDDEN_VOICE_CHAT_FIELDS = new Set([
  "AppKey",
  "AccessKeyId",
  "SecretAccessKey",
  "Token",
  "Authorization",
]);

const ALLOWED_RUNTIME_CREDENTIAL_PATHS = new Set([
  "voice_chat.Config.ASRConfig.ProviderParams.AppId",
  "voice_chat.Config.ASRConfig.ProviderParams.AccessToken",
]);

function rejectForbiddenVoiceChatFields(value, path = "voice_chat") {
  if (Array.isArray(value)) {
    value.forEach((item, index) =>
      rejectForbiddenVoiceChatFields(item, `${path}[${index}]`)
    );
    return;
  }
  if (!value || typeof value !== "object") {
    return;
  }
  for (const [key, item] of Object.entries(value)) {
    const itemPath = `${path}.${key}`;
    const isDynamicRootField =
      path === "voice_chat" && ["AppId", "RoomId", "TaskId"].includes(key);
    const isScopedRuntimeCredential = ["AppId", "AccessToken"].includes(key);
    if (
      isDynamicRootField ||
      FORBIDDEN_VOICE_CHAT_FIELDS.has(key) ||
      (isScopedRuntimeCredential && !ALLOWED_RUNTIME_CREDENTIAL_PATHS.has(itemPath))
    ) {
      throw new Error(`VoiceChat config must not set ${itemPath}`);
    }
    rejectForbiddenVoiceChatFields(item, itemPath);
  }
}

function loadVoiceChatConfig(configPath, env) {
  const raw = fs.readFileSync(configPath, "utf8");
  const value = expandEnvReferences(JSON.parse(raw), env);
  if (!value?.Config || !value?.AgentConfig) {
    throw new Error("VoiceChat config must contain Config and AgentConfig");
  }
  rejectForbiddenVoiceChatFields(value);
  return value;
}

function loadRuntimeConfig(env = process.env, serviceRoot = path.resolve(__dirname, "..")) {
  const voiceChatPath = path.resolve(
    serviceRoot,
    env.VOICE_CHAT_CONFIG_PATH?.trim() || "config/voice_chat.json"
  );
  const deviceApiKey = required(env.DEVICE_API_KEY, "DEVICE_API_KEY");
  if (Buffer.byteLength(deviceApiKey) < 24) {
    throw new Error("DEVICE_API_KEY must be at least 24 bytes");
  }

  const config = {
    appId: required(env.RTC_APP_ID, "RTC_APP_ID"),
    appKey: required(env.RTC_APP_KEY, "RTC_APP_KEY"),
    accessKeyId: required(
      env.VOLCENGINE_ACCESS_KEY_ID,
      "VOLCENGINE_ACCESS_KEY_ID"
    ),
    secretAccessKey: required(
      env.VOLCENGINE_SECRET_ACCESS_KEY,
      "VOLCENGINE_SECRET_ACCESS_KEY"
    ),
    businessId: env.RTC_BUSINESS_ID?.trim() || undefined,
    deviceApiKey,
    host: env.HOST?.trim() || "0.0.0.0",
    port: parseInteger(env.PORT, "PORT", 8080, 1, 65535),
    sessionTtlSeconds: parseInteger(
      env.RTC_SESSION_TTL_SECONDS,
      "RTC_SESSION_TTL_SECONDS",
      3600,
      300,
      86400
    ),
    voiceChatPath,
  };
  config.voiceChat = loadVoiceChatConfig(voiceChatPath, env);
  return config;
}

module.exports = {
  expandEnvReferences,
  loadEnvFile,
  loadRuntimeConfig,
  loadVoiceChatConfig,
  rejectForbiddenVoiceChatFields,
};
