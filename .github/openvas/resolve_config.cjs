"use strict";

const fs = require("node:fs/promises");

const MAX_RESPONSE_BYTES = 64 * 1024;

// Purpose: Identify diagnostics that contain only fixed field names, never broker values.
// Inputs: A locally constructed diagnostic. Outputs: A safe configuration error.
class ConfigurationError extends Error {}

// Purpose: Decode bounded broker configuration without coercing compound objects or control characters.
// Inputs: A parsed, untrusted JSON value. Outputs: Validated single-line fields or a value-free error.
function parseConfiguration(config) {
    if (config === null || typeof config !== "object" || Array.isArray(config)) {
        throw new ConfigurationError("Greenbone broker response must be a JSON object");
    }
    const get = (name, fallback = "") => {
        const raw = config[name] ?? fallback;
        if (!["string", "number", "boolean"].includes(typeof raw)) {
            throw new ConfigurationError(`Greenbone broker field ${name} must be scalar`);
        }
        const value = String(raw);
        if (value.length > 4096 || /[\0\r\n]/u.test(value)) {
            throw new ConfigurationError(`Greenbone broker field ${name} must be bounded and single-line`);
        }
        return value;
    };
    const values = {
        host: get("greenbone_host"),
        username: get("greenbone_username"),
        password: get("greenbone_password"),
        target: get("greenbone_target"),
        port: get("greenbone_port", "9390"),
        max_minutes: get("greenbone_max_minutes", "180"),
        delete_task: get("greenbone_delete_task", "true"),
        scan_config_id: get("greenbone_scan_config_id", "daba56c8-73ec-11df-a475-002264764cea"),
        scanner_id: get("greenbone_scanner_id", "08b69003-5fc2-4037-a479-93b440211c73"),
        port_list_id: get("greenbone_port_list_id"),
        vulnetix_org_id: get("vulnetix_org_id"),
    };
    const missing = ["host", "username", "password", "target", "vulnetix_org_id"].filter((key) => !values[key]);
    if (missing.length !== 0) {
        throw new ConfigurationError(`Greenbone broker omitted required fields: ${missing.join(", ")}`);
    }
    return values;
}

// Purpose: Bound response allocation before parsing private broker data.
// Inputs: A successful Fetch response. Outputs: Parsed JSON or a value-free diagnostic; cancels excess input.
async function readConfiguration(response) {
    if (!response.ok || response.body === null) {
        throw new ConfigurationError("Greenbone broker rejected the configuration request");
    }
    const reader = response.body.getReader();
    const chunks = [];
    let size = 0;
    try {
        for (;;) {
            const next = await reader.read();
            if (next.done) break;
            size += next.value.byteLength;
            if (size > MAX_RESPONSE_BYTES) {
                throw new ConfigurationError("Greenbone broker response exceeds the configuration limit");
            }
            chunks.push(next.value);
        }
        return JSON.parse(new TextDecoder("utf-8", { fatal: true }).decode(Buffer.concat(chunks)));
    } catch (error) {
        if (error instanceof ConfigurationError) throw error;
        throw new ConfigurationError("Greenbone broker returned unreadable configuration JSON");
    } finally {
        await reader.cancel().catch(() => {});
        reader.releaseLock();
    }
}

// Purpose: Resolve broker-authorized settings using official Actions token, masking, and output APIs.
// Inputs: Actions core plus injectable environment/Fetch/report writer for offline tests.
// Outputs: Masks all values before exposing step outputs; reports fixed diagnostics and fails closed on errors.
async function resolveConfig(core, environment = process.env, fetcher = globalThis.fetch, writer = fs) {
    try {
        const providerText = environment.GREENBONE_SECRET_PROVIDER_URL || "";
        if (!providerText) {
            throw new ConfigurationError("GREENBONE_SECRET_PROVIDER_URL repository variable is required");
        }
        const provider = new URL(providerText);
        const audience = environment.GREENBONE_SECRET_PROVIDER_AUDIENCE || "superzip-openvas";
        const targetRequest = environment.GREENBONE_TARGET_INPUT || "";
        if (provider.protocol !== "https:" || provider.username || provider.password || provider.hash ||
            [providerText, audience, targetRequest].some((value) => value.length > 4096 || /[\0\r\n]/u.test(value))) {
            throw new ConfigurationError("Greenbone broker routing must use HTTPS and bounded single-line values");
        }
        const token = await core.getIDToken(audience);
        const response = await fetcher(provider, {
            method: "POST",
            redirect: "error",
            signal: AbortSignal.timeout(30000),
            headers: { Authorization: `Bearer ${token}`, "Content-Type": "application/json" },
            body: JSON.stringify({
                repository: environment.GITHUB_REPOSITORY,
                workflow: environment.GITHUB_WORKFLOW,
                ref: environment.GITHUB_REF,
                sha: environment.GITHUB_SHA,
                run_id: environment.GITHUB_RUN_ID,
                event_name: environment.GITHUB_EVENT_NAME,
                target_input: targetRequest,
            }),
        });
        const values = parseConfiguration(await readConfiguration(response));
        for (const value of Object.values(values)) {
            if (value) core.setSecret(value);
        }
        for (const [key, value] of Object.entries(values)) core.setOutput(key, value);
    } catch (error) {
        const message = error instanceof ConfigurationError ? error.message : "Greenbone configuration request failed";
        try {
            await writer.mkdir("reports/openvas", { recursive: true });
            await writer.writeFile("reports/openvas/openvas-configuration-missing.json", JSON.stringify({
                tool: "Greenbone/OpenVAS", status: "configuration-invalid", message,
            }, null, 2), { encoding: "utf-8" });
        } catch {
            // Filesystem exceptions can contain private host paths; the original failure remains authoritative.
        }
        core.setFailed(message);
    }
}

module.exports = { parseConfiguration, readConfiguration, resolveConfig };
