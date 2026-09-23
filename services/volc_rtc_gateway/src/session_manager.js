const crypto = require("node:crypto");
const { createRtcToken } = require("./rtc_token");

const START_ACTION = "StartVoiceChat";
const STOP_ACTION = "StopVoiceChat";
// Volcengine's embedded OPUS client uses this room prefix to select the
// low-bitrate OPUS transport policy for the room.
const OPUS_ROOM_PREFIX = "OPUS";
const DEVICE_ID_PATTERN = /^[A-Za-z0-9._:-]{1,64}$/;

class CloudApiError extends Error {
  constructor(action, response) {
    const cloudError = response?.ResponseMetadata?.Error;
    const cloudCode =
      cloudError?.Code ||
      (response?.Result ? `UNEXPECTED_RESULT_${response.Result}` : "MISSING_RESULT");
    super(`${action} failed: ${cloudCode}`);
    this.name = "CloudApiError";
    this.action = action;
    this.cloudCode = cloudCode;
    this.requestId = response?.ResponseMetadata?.RequestId;
  }
}

function clone(value) {
  return JSON.parse(JSON.stringify(value));
}

function responseSucceeded(response) {
  return (
    Boolean(response?.ResponseMetadata) &&
    !response.ResponseMetadata.Error &&
    response.Result === "ok"
  );
}

class SessionManager {
  constructor(config, invokeOpenApi, logger = console) {
    this.config = config;
    this.invokeOpenApi = invokeOpenApi;
    this.logger = logger;
    this.sessions = new Map();
    this.deviceSessions = new Map();
    this.deviceOperations = new Map();
  }

  async runDeviceOperation(deviceId, operation) {
    const previous = this.deviceOperations.get(deviceId) || Promise.resolve();
    let release;
    const current = new Promise((resolve) => {
      release = resolve;
    });
    this.deviceOperations.set(deviceId, current);
    await previous.catch(() => {});
    try {
      return await operation();
    } finally {
      release();
      if (this.deviceOperations.get(deviceId) === current) {
        this.deviceOperations.delete(deviceId);
      }
    }
  }

  buildStartRequest(session) {
    const request = clone(this.config.voiceChat);
    request.AppId = this.config.appId;
    request.RoomId = session.roomId;
    request.TaskId = session.taskId;
    if (this.config.businessId) {
      request.BusinessId = this.config.businessId;
    }
    request.AgentConfig.TargetUserId = [session.userId];
    request.AgentConfig.UserId = session.botUserId;
    request.AgentConfig.EnableConversationStateCallback = true;
    request.AgentConfig.Burst ??= {
      Enable: false,
      BufferSize: 500,
      Interval: 20,
    };
    return request;
  }

  async start(deviceId) {
    if (!DEVICE_ID_PATTERN.test(deviceId || "")) {
      throw new TypeError("device_id must contain 1-64 safe ASCII characters");
    }

    return this.runDeviceOperation(deviceId, () => this.startUnlocked(deviceId));
  }

  async startUnlocked(deviceId) {
    const previousId = this.deviceSessions.get(deviceId);
    if (previousId) {
      try {
        await this.stopUnlocked(previousId);
      } catch (error) {
        this.logger.warn("failed to stop previous RTC session", {
          deviceId,
          error: error.message,
        });
      }
    }

    const now = Math.floor(Date.now() / 1000);
    const session = {
      id: crypto.randomUUID(),
      deviceId,
      roomId: `${OPUS_ROOM_PREFIX}${crypto
        .randomUUID()
        .replaceAll("-", "")}`,
      userId: `esp_${crypto
        .createHash("sha256")
        .update(deviceId)
        .digest("hex")
        .slice(0, 12)}_${crypto.randomUUID().replaceAll("-", "").slice(0, 8)}`,
      botUserId: `bot_${crypto.randomUUID().replaceAll("-", "")}`,
      taskId: crypto.randomUUID(),
      expiresAt: now + this.config.sessionTtlSeconds,
    };
    const response = await this.invokeOpenApi(
      START_ACTION,
      this.buildStartRequest(session)
    );
    if (!responseSucceeded(response)) {
      throw new CloudApiError(START_ACTION, response);
    }
    this.logger.info("VoiceChat task accepted", {
      deviceId,
      roomPolicy: OPUS_ROOM_PREFIX,
      requestId: response.ResponseMetadata.RequestId,
    });

    this.sessions.set(session.id, session);
    this.deviceSessions.set(deviceId, session.id);
    return {
      session_id: session.id,
      app_id: this.config.appId,
      room_id: session.roomId,
      user_id: session.userId,
      token: createRtcToken({
        appId: this.config.appId,
        appKey: this.config.appKey,
        roomId: session.roomId,
        userId: session.userId,
        expiresAt: session.expiresAt,
      }),
      expires_at: session.expiresAt,
    };
  }

  async stop(sessionId) {
    const session = this.sessions.get(sessionId);
    if (!session) {
      return false;
    }
    return this.runDeviceOperation(session.deviceId, () =>
      this.stopUnlocked(sessionId)
    );
  }

  async stopUnlocked(sessionId) {
    const session = this.sessions.get(sessionId);
    if (!session) {
      return false;
    }
    const body = {
      AppId: this.config.appId,
      RoomId: session.roomId,
      TaskId: session.taskId,
    };
    const response = await this.invokeOpenApi(STOP_ACTION, body);
    if (!responseSucceeded(response)) {
      throw new CloudApiError(STOP_ACTION, response);
    }
    this.sessions.delete(session.id);
    if (this.deviceSessions.get(session.deviceId) === session.id) {
      this.deviceSessions.delete(session.deviceId);
    }
    return true;
  }

  async reapExpired(now = Math.floor(Date.now() / 1000)) {
    const expiredIds = [...this.sessions.values()]
      .filter((session) => session.expiresAt <= now)
      .map((session) => session.id);
    await Promise.allSettled(expiredIds.map((id) => this.stop(id)));
    return expiredIds.length;
  }

  async close() {
    await Promise.allSettled([...this.sessions.keys()].map((id) => this.stop(id)));
  }
}

module.exports = {
  CloudApiError,
  SessionManager,
  OPUS_ROOM_PREFIX,
  START_ACTION,
  STOP_ACTION,
};
