"use strict";

const assert = require("node:assert/strict");
const { test } = require("node:test");
const { parseConfiguration, readConfiguration, resolveConfig } = require("./resolve_config.cjs");

const valid = {
    greenbone_host: "scanner.example",
    greenbone_username: "test-user",
    greenbone_password: "fixture%0Avalue%25with::markers",
    greenbone_target: "authorized.example",
    vulnetix_org_id: "fixture-organization",
};

// Purpose: Capture resolver effects without real credentials, files, or network traffic.
// Inputs: A broker fixture or injected failure. Outputs: Recorded core calls, request, and failure reports.
async function exercise(config = valid, overrides = {}) {
    const calls = [];
    const reports = [];
    let request;
    const core = {
        getIDToken: async (audience) => { calls.push(["token", audience]); return "synthetic-oidc-fixture"; },
        setSecret: (value) => calls.push(["mask", value]),
        setOutput: (key, value) => calls.push(["output", key, value]),
        setFailed: (message) => calls.push(["failed", message]),
        ...overrides.core,
    };
    const env = { GREENBONE_SECRET_PROVIDER_URL: "https://broker.example/config",
        GREENBONE_TARGET_INPUT: "requested.example", ...overrides.env };
    const fetcher = overrides.fetcher || (async (url, options) => {
        request = { url, ...options };
        return new Response(JSON.stringify(config));
    });
    const writer = { mkdir: async () => {}, writeFile: async (_path, value) => reports.push(value), ...overrides.writer };
    await resolveConfig(core, env, fetcher, writer);
    return { calls, reports, request };
}

test("official masking receives exact values before any outputs and target stays broker-authorized", async () => {
    const result = await exercise();
    const outputs = result.calls.filter(([kind]) => kind === "output");
    assert.equal(outputs.find(([, key]) => key === "target")[2], "authorized.example");
    assert.equal(outputs.find(([, key]) => key === "password")[2], valid.greenbone_password);
    assert.ok(result.calls.some(([kind, value]) => kind === "mask" && value === valid.greenbone_password));
    const firstOutput = result.calls.findIndex(([kind]) => kind === "output");
    assert.ok(result.calls.slice(firstOutput).every(([kind]) => kind === "output"));
    assert.equal(JSON.parse(result.request.body).target_input, "requested.example");
    assert.equal(result.request.redirect, "error");
    assert.ok(result.request.signal instanceof AbortSignal);
    assert.equal(result.reports.length, 0);
});

test("invalid, missing, and compound fields fail before any output", async () => {
    for (const config of [null, [], {}, { ...valid, greenbone_password: {} },
        { ...valid, greenbone_target: "" }, { ...valid, greenbone_password: "x\r\ny" },
        { ...valid, greenbone_host: "x\0y" }, { ...valid, greenbone_password: "x".repeat(4097) }]) {
        const result = await exercise(config);
        assert.ok(result.calls.some(([kind]) => kind === "failed"));
        assert.ok(!result.calls.some(([kind]) => kind === "output" || kind === "mask"));
        assert.equal(result.reports.length, 1);
    }
});

test("unsafe routing is rejected before requesting an OIDC token", async () => {
    const credentialed = new URL("https://broker.example/");
    credentialed.username = "fixture-user";
    credentialed.password = "fixture-password";
    for (const url of ["", "http://broker.example/", credentialed.href, "https://broker.example/#secret",
        "https://broker.example/\n"]) {
        const result = await exercise(valid, { env: { GREENBONE_SECRET_PROVIDER_URL: url } });
        assert.ok(!result.calls.some(([kind]) => kind === "token" || kind === "output"));
        assert.ok(result.calls.some(([kind]) => kind === "failed"));
    }
});

test("network and token errors never echo private error details", async () => {
    for (const overrides of [
        { fetcher: async () => { throw new Error(valid.greenbone_password); } },
        { core: { getIDToken: async () => { throw new Error(valid.greenbone_password); } } },
    ]) {
        const result = await exercise(valid, overrides);
        assert.ok(result.calls.some(([kind]) => kind === "failed"));
        assert.ok(!JSON.stringify(result).includes(valid.greenbone_password));
    }
});

test("report writer failures cannot expose private exception text or prevent a failed step", async () => {
    for (const method of ["mkdir", "writeFile"]) {
        const writer = { [method]: async () => { throw new Error("private-filesystem-detail"); } };
        const result = await exercise(null, { writer });
        assert.ok(result.calls.some(([kind]) => kind === "failed"));
        assert.ok(!JSON.stringify(result).includes("private-filesystem-detail"));
    }
});

test("masking failures stop before any step output", async () => {
    const result = await exercise(valid, {
        core: { setSecret: () => { throw new Error(valid.greenbone_password); } },
    });
    assert.ok(result.calls.some(([kind]) => kind === "failed"));
    assert.ok(!result.calls.some(([kind]) => kind === "output"));
    assert.ok(!JSON.stringify(result).includes(valid.greenbone_password));
});

test("unreadable broker responses produce value-free errors", async () => {
    for (const response of [new Response(valid.greenbone_password), new Response(new Uint8Array([255])),
        new Response(valid.greenbone_password, { status: 403 }), new Response("x".repeat(65537))]) {
        await assert.rejects(readConfiguration(response), (error) => !error.message.includes(valid.greenbone_password));
    }
});

test("exact response ceiling and legacy scalar defaults remain accepted", async () => {
    const text = JSON.stringify(valid);
    const padded = text + " ".repeat(65536 - Buffer.byteLength(text));
    assert.deepEqual(await readConfiguration(new Response(padded)), valid);
    const fields = parseConfiguration({ ...valid, greenbone_port: 9390, greenbone_delete_task: true });
    assert.equal(fields.port, "9390");
    assert.equal(fields.delete_task, "true");
    assert.equal(fields.max_minutes, "180");
});
