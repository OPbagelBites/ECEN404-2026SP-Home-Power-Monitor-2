const admin = require("firebase-admin");
admin.initializeApp();

const {onValueCreated} = require("firebase-functions/v2/database");
const {defineString} = require("firebase-functions/params");

const OHM_PREDICT_URL = defineString("OHM_PREDICT_URL");
const OHM_MODEL_NAME = defineString("OHM_MODEL_NAME");

exports.predictOnNewFrame = onValueCreated(
    "/devices/{deviceId}/streams/{fw}/frames/{frameId}",
    async (event) => {
      const frame = event.data.val();
      const path = event.data.ref.toString();

      if (!frame) {
        console.log("TRIGGERED but frame was empty", {path});
        return;
      }

      const receivedTs = Date.now();
      const receivedCt = new Date(receivedTs).toLocaleString("en-US", {
        timeZone: "America/Chicago",
      });

      // loop protection
      if (frame.ml) {
        console.log("SKIP: ml already exists", {path});
        return;
      }

      const p = Math.abs(Number(frame.p ?? 0));
      const rmsI = Number(frame.rms_i ?? 0);

      // OFF gate
      if (p < 10 && rmsI < 0.05) {
        console.log("OFF: skipping inference", {path, p, rmsI});
        await event.data.ref.child("ml").set({
          status: "done",
          pred: ["Off"],
          model: OHM_MODEL_NAME.value() || "ohm-ml-api",
          received_ts: receivedTs,
          received_ct: receivedCt,
          ts: Date.now(),
        });
        return;
      }

      const url = OHM_PREDICT_URL.value();
      const modelName = OHM_MODEL_NAME.value() || "ohm-ml-api";

      if (!url) {
        console.error("Missing env var OHM_PREDICT_URL", {path});
        await event.data.ref.child("ml").set({
          status: "error",
          error: "Missing env var OHM_PREDICT_URL",
          model: modelName,
          received_ts: receivedTs,
          received_ct: receivedCt,
          ts: Date.now(),
        });
        return;
      }

      const hI = frame.h_i || {};
      const pfTrue = Math.abs(Number(frame.pf_true ?? 1));

      // send raw debug samples too so feature_patch can recompute harmonics
      const payload = {
        p,
        pf_true: pfTrue,
        rms_i: frame.rms_i ?? 0,
        crest_i: frame.crest_i ?? 0,
        thd_i: frame.thd_i,
        s: frame.s,

        // harmonic aliases if present
        h2_i_norm: hI["120"] ?? 0,
        h3_i_norm: hI["180"] ?? 0,
        h4_i_norm: hI["240"] ?? 0,
        h5_i_norm: hI["300"] ?? 0,

        // raw frame context for feature_patch.py
        freq_hz: frame.freq_hz ?? 60,
        rms_v: frame.rms_v ?? 120,
        dbg: {
          i_samp: frame.dbg?.i_samp ?? [],
          fs_eff: frame.dbg?.fs_eff ?? frame.fs ?? 996.82,
        },
      };

      console.log("TRIGGERED", {
        path,
        p: payload.p,
        pf_true: payload.pf_true,
        hasHarmonics: !!frame.h_i,
        hasDbgSamples: (payload.dbg.i_samp || []).length > 0,
      });

      console.log("Calling Cloud Run", {url, payload});

      try {
        const resp = await fetch(url, {
          method: "POST",
          headers: {"Content-Type": "application/json"},
          body: JSON.stringify(payload),
        });

        const data = await resp.json();

        if (!resp.ok) {
          console.error("Cloud Run non-200", {status: resp.status, data});
          throw new Error(`Cloud Run ${resp.status}: ${JSON.stringify(data)}`);
        }

        console.log("Cloud Run response", data);

        await event.data.ref.child("ml").set({
          status: "done",
          pred: data.appliance_predictions ?? [],
          raw: data,
          model: modelName,
          received_ts: receivedTs,
          received_ct: receivedCt,
          ts: Date.now(),
        });
      } catch (err) {
        console.error("ERROR calling Cloud Run / writing ml", {
          path,
          err: String(err),
        });

        await event.data.ref.child("ml").set({
          status: "error",
          error: String(err),
          model: modelName,
          received_ts: receivedTs,
          received_ct: receivedCt,
          ts: Date.now(),
        });
      }
    },
);
