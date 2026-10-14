#!/usr/bin/env node
/*
	Compare two -netdump traces: a host one and a client one.

	Both write one CSV row per networked object per host tick. This lines the
	two up on (tick, kind, key) and reports:

	  R  racer as it travels on the wire. Host packed it, client unpacked it.
	     Matched rows must be byte-identical; the only thing this can show is
	     how many snapshots never arrived (and which racers were in them).
	  W  client world, host authoritative R. The client's remote cars are
	     interpolated a fixed delay behind the host and its own car is
	     simulated locally, so this is compared at the lag that fits best,
	     and the residual after that lag is the real drift.
	  E  replicated bodies (projectiles, actors, walking bonuses), host world
	     vs client world. Grouped by type and variant, so BonusMan1..4 are
	     separate. Reports three things the user cares about separately:
	       - count:   does the client hold the same number of objects per tick?
	       - props:   do type/variant/model/alive agree for the same id?
	       - pose:    how far apart are matched copies (lag-fitted residual)?

	     Covered: every type net_fx.cpp registers.
	       class B: cannon_bomb, volcanic_bomb, fire_bolide, snow_ball,
	         sleepy_mortar, minor_bonus, major_bonus, titanium_ball, and the
	         factory-built smart bodies (smart_fireball, smart_lightningbolt,
	         suicide, clown, evil_eye, tomato, crystal, bubble, little_spider);
	       class C: bonus_man#1..#4 (the walking bonuses), butterfly,
	         wicked_butterfly, caterpillar, worm, dragon, frog, shark, fish.

	     Not covered, and why:
	       - racer bodies: the R/W sections;
	       - mechos parts and wheels: identical from the same mechos config,
	         only their arcane power/status travels, already in R;
	       - static track scenery (bonus boxes, gears, stones) and debris:
	         built on both machines from the same track load, never sent;
	       - class D cosmetics (dust, trails, particles) and sound: local,
	         drawn from the shared fxlabClientRND stream;
	       - areas (trap_sphere, red_track, slime_circle, damage_sensor):
	         declared in netEntityTypes but not registered by net_fx, so
	         nothing replicates them yet.

	Usage:
	  node tools/net-trace-compare.js <host.csv> <client.csv> [localSlot]

	Exit code is always 0: this is a report, not a gate.
*/

'use strict';

const fs = require('fs');

const COLS = ['tick','kind','key','type','variant','px','py','pz','yaw','vx','vy','vz',
	'speed','energy','flags','lap','cp','place','nstar','mech','feat',
	'pw0','pw1','pw2','st0','st1','st2','model','alive'];

const ENT_NAMES = {
	1:'cannon_bomb',2:'volcanic_bomb',3:'fire_bolide',4:'snow_ball',5:'sleepy_mortar',
	6:'minor_bonus',7:'major_bonus',8:'debris',9:'titanium_ball',10:'crystal',11:'bubble',
	12:'smart_fireball',13:'smart_lightningbolt',14:'suicide',15:'clown',16:'evil_eye',
	17:'tomato',18:'little_spider',
	19:'bonus_man',20:'butterfly',21:'wicked_butterfly',22:'dragon',23:'shark',24:'whale',
	25:'frog',26:'worm',27:'caterpillar',28:'fish',
	29:'trap_sphere',30:'red_track',31:'slime_circle',32:'damage_sensor',33:'force_field',
};
// Entity types below BONUS_MAN are class B (independently simulated), above
// are class C (state-replicated).
const entIsActor = t => t >= 19 && t <= 33;
// Walking bonuses are the four BonusMan subclasses, one enum value, split by
// variant. This is the label the report shows for them.
function entLabel(type, variant) {
	const base = ENT_NAMES[type] || ('type' + type);
	return variant ? base + '#' + variant : base;
}

const LAG_MAX = 8;

function load(path) {
	const text = fs.readFileSync(path, 'utf8');
	const lines = text.split(/\r?\n/);
	const header = lines.shift();
	if (!header || !header.startsWith('tick,')) throw new Error(path + ': missing header');
	const idx = {};
	header.split(',').forEach((c, i) => { idx[c] = i; });
	const rows = [];
	for (const line of lines) {
		if (!line) continue;
		const f = line.split(',');
		if (f.length < COLS.length) continue;
		rows.push({
			tick: +f[idx.tick], kind: f[idx.kind], key: +f[idx.key], type: +f[idx.type],
			variant: +f[idx.variant],
			px: +f[idx.px], py: +f[idx.py], pz: +f[idx.pz], yaw: +f[idx.yaw],
			vx: +f[idx.vx], vy: +f[idx.vy], vz: +f[idx.vz],
			speed: +f[idx.speed], energy: +f[idx.energy], flags: +f[idx.flags],
			lap: +f[idx.lap], cp: +f[idx.cp], place: +f[idx.place], nstar: +f[idx.nstar],
			mech: +f[idx.mech], feat: +f[idx.feat],
			pw: [+f[idx.pw0], +f[idx.pw1], +f[idx.pw2]],
			st: [+f[idx.st0], +f[idx.st1], +f[idx.st2]],
			model: +f[idx.model], alive: +f[idx.alive],
		});
	}
	return rows;
}

const byKind = (rows, kind) => rows.filter(r => r.kind === kind);
const keyOf = r => r.tick + '|' + r.key;
const dist2 = (a, b) => Math.hypot(a.px - b.px, a.py - b.py);
const dist3 = (a, b) => Math.hypot(a.px - b.px, a.py - b.py, a.pz - b.pz);
function angdiff(a, b) {
	let d = a - b;
	while (d > Math.PI) d -= 2 * Math.PI;
	while (d < -Math.PI) d += 2 * Math.PI;
	return Math.abs(d);
}
function stats(vals) {
	if (!vals.length) return { n: 0, mean: 0, p50: 0, p95: 0, max: 0 };
	const s = [...vals].sort((a, b) => a - b);
	const sum = s.reduce((a, b) => a + b, 0);
	return {
		n: s.length, mean: sum / s.length,
		p50: s[Math.floor(s.length * 0.5)],
		p95: s[Math.min(s.length - 1, Math.floor(s.length * 0.95))],
		max: s[s.length - 1],
	};
}
const f = (v, w = 7) => (Number.isFinite(v) ? v.toFixed(2) : '  n/a ').padStart(w);

// The lag that makes a client series line up with the host series best.
function bestLag(clientRows, hostMap) {
	let best = { lag: 0, mean: Infinity };
	for (let lag = -LAG_MAX; lag <= LAG_MAX; lag++) {
		const ds = [];
		for (const cr of clientRows) {
			const h = hostMap.get((cr.tick + lag) + '|' + cr.key);
			if (h) ds.push(dist2(cr, h));
		}
		if (ds.length < 10) continue;
		const mean = ds.reduce((a, b) => a + b, 0) / ds.length;
		if (mean < best.mean) best = { lag, mean };
	}
	return best;
}

function main() {
	const [, , hostPath, clientPath, slotArg] = process.argv;
	if (!hostPath || !clientPath) {
		console.error('usage: node tools/net-trace-compare.js <host.csv> <client.csv> [localSlot]');
		process.exit(2);
	}
	const localSlot = slotArg !== undefined ? +slotArg : 1;

	const host = load(hostPath);
	const client = load(clientPath);
	console.log('# host   ' + hostPath + '  (' + host.length + ' rows)');
	console.log('# client ' + clientPath + '  (' + client.length + ' rows)');

	/* ------------------------------ R: on the wire ------------------------- */
	const hR = byKind(host, 'R'), cR = byKind(client, 'R');
	if (hR.length && cR.length) {
		const hMap = new Map(hR.map(r => [keyOf(r), r]));
		const cMap = new Map(cR.map(r => [keyOf(r), r]));
		const hTicks = new Set(hR.map(r => r.tick));
		const cTicks = new Set(cR.map(r => r.tick));
		let lostTicks = 0, extraTicks = 0;
		for (const t of hTicks) if (!cTicks.has(t)) lostTicks++;
		for (const t of cTicks) if (!hTicks.has(t)) extraTicks++;

		let matched = 0, mismatched = 0;
		const fieldBad = {};
		for (const [k, cr] of cMap) {
			const hr = hMap.get(k);
			if (!hr) continue;
			matched++;
			let bad = false;
			for (const fld of ['px','py','pz','yaw','vx','vy','vz','speed','energy','flags','lap','cp','place','nstar','mech','feat']) {
				if (Math.abs(hr[fld] - cr[fld]) > 1e-3) { bad = true; fieldBad[fld] = (fieldBad[fld] || 0) + 1; }
			}
			for (let i = 0; i < 3; i++) {
				if (hr.pw[i] !== cr.pw[i]) { bad = true; fieldBad['pw' + i] = (fieldBad['pw' + i] || 0) + 1; }
				if (hr.st[i] !== cr.st[i]) { bad = true; fieldBad['st' + i] = (fieldBad['st' + i] || 0) + 1; }
			}
			if (bad) mismatched++;
		}
		console.log('\n== R: racer on the wire ==');
		console.log('  host ticks ' + hTicks.size + ', client ticks ' + cTicks.size +
			', client ticks absent on host: ' + extraTicks);
		console.log('  snapshot ticks sent by host but absent on client: ' + lostTicks +
			' (' + (100 * lostTicks / Math.max(1, hTicks.size)).toFixed(1) + '% loss)');
		console.log('  received rows also sent by host: ' + matched +
			', of which field-mismatched: ' + mismatched);
		if (Object.keys(fieldBad).length) console.log('  mismatched fields: ' + JSON.stringify(fieldBad));
	} else {
		console.log('\n== R: racer on the wire == (no R rows)');
	}

	/* ------------------- W: client world vs host authoritative -------------- */
	const cW = byKind(client, 'W');
	if (cW.length && hR.length) {
		console.log('\n== W: client world vs host authoritative ==');
		console.log('  local slot ' + localSlot + ' (client-driven); other slots are remote, interpolated');
		const hBySlotTick = new Map(hR.map(r => [r.key + '|' + r.tick, r]));
		const slots = [...new Set(cW.map(r => r.key))].sort((a, b) => a - b);
		for (const slot of slots) {
			const rows = cW.filter(r => r.key === slot);
			const hSlot = new Map();
			for (const h of hR) if (h.key === slot) hSlot.set(h.tick + '|' + slot, h);
			const best = bestLag(rows, hSlot);
			const ds2 = [], ds3 = [], dyaw = [];
			for (const w of rows) {
				const h = hBySlotTick.get(slot + '|' + (w.tick + best.lag));
				if (!h) continue;
				ds2.push(dist2(w, h));
				ds3.push(dist3(w, h));
				dyaw.push(angdiff(w.yaw, h.yaw));
			}
			const s2 = stats(ds2);
			const tag = (slot === localSlot) ? 'local ' : 'remote';
			console.log('  slot ' + slot + ' ' + tag +
				'  rows ' + rows.length +
				'  best lag ' + String(best.lag).padStart(3) + 't' +
				'  xy mean ' + f(s2.mean) + ' p50 ' + f(s2.p50) + ' p95 ' + f(s2.p95) +
				' max ' + f(s2.max) +
				'  3d max ' + f(stats(ds3).max) +
				'  yaw max ' + f(stats(dyaw).max, 6));
		}
	} else {
		console.log('\n== W: client world vs host authoritative == (no W rows)');
	}

	/* --------------------- E: replicated bodies, world vs world ------------- */
	const hE = byKind(host, 'E'), cE = byKind(client, 'E');
	if (hE.length || cE.length) {
		const hMap = new Map(hE.map(r => [keyOf(r), r]));
		const cMap = new Map(cE.map(r => [keyOf(r), r]));

		// Group label. Walking bonuses carry a variant and show as bonus_man#N.
		const label = r => entLabel(r.type, r.variant);

		// Property agreement for the same (tick, id): does the client's idea of
		// what the object is match the host's?
		const propBad = { type: 0, variant: 0, model: 0, alive: 0 };
		let propMatched = 0, hostOnly = 0, clientOnly = 0;
		for (const [k, hr] of hMap) {
			const cr = cMap.get(k);
			if (!cr) { hostOnly++; continue; }
			propMatched++;
			if (hr.type !== cr.type) propBad.type++;
			if (hr.variant !== cr.variant) propBad.variant++;
			if (hr.model !== cr.model) propBad.model++;
			if (hr.alive !== cr.alive) propBad.alive++;
		}
		for (const [k] of cMap) if (!hMap.has(k)) clientOnly++;

		console.log('\n== E: replicated bodies, host world vs client world ==');
		console.log('  host rows ' + hE.length + ', client rows ' + cE.length +
			', same (tick,id) ' + propMatched);
		console.log('  present on host only ' + hostOnly + ', on client only ' + clientOnly +
			' (spawn/despawn timing skew)');
		console.log('  property mismatch on same (tick,id): type ' + propBad.type +
			', variant ' + propBad.variant + ', model ' + propBad.model +
			', alive ' + propBad.alive);

		// Per-tick object count per label, both sides.
		const hCount = new Map(), cCount = new Map();
		function bump(map, r) {
			const g = label(r);
			if (!map.has(g)) map.set(g, new Map());
			const t = map.get(g);
			t.set(r.tick, (t.get(r.tick) || 0) + 1);
		}
		for (const r of hE) bump(hCount, r);
		for (const r of cE) bump(cCount, r);

		// A tick the client never received has no client trace at all, so its
		// object count is unknowable, not zero. Counts are compared only on
		// ticks present in the client's own R trace; the rest are counted as
		// skipped so packet loss cannot be mistaken for a missing object.
		const recvTicks = new Set(cR.map(r => r.tick));

		const groups = new Set([...hCount.keys(), ...cCount.keys()]);
		const report = [];
		for (const g of groups) {
			const ht = hCount.get(g) || new Map();
			const ct = cCount.get(g) || new Map();
			const ticks = new Set([...ht.keys(), ...ct.keys()]);
			let diffTicks = 0, maxDiff = 0, sumDiff = 0, hTotal = 0, cTotal = 0, skipped = 0, cmp = 0;
			for (const t of ticks) {
				if (!recvTicks.has(t)) { skipped++; continue; }
				cmp++;
				const a = ht.get(t) || 0, b = ct.get(t) || 0;
				hTotal += a; cTotal += b;
				const d = Math.abs(a - b);
				if (d) diffTicks++;
				if (d > maxDiff) maxDiff = d;
				sumDiff += d;
			}
			// Pose residual for this label, at the label's best lag.
			const rows = cE.filter(r => label(r) === g);
			const hKey = new Map(hE.filter(r => label(r) === g).map(r => [(r.tick) + '|' + r.key, r]));
			const best = bestLag(rows, hKey);
			const d2 = [], d3 = [], dyaw = [], dv = [], ages = [];
			const big = [0, 0, 0];
			const firstSeen = new Map();
			for (const r of hE) if (label(r) === g && !firstSeen.has(r.key)) firstSeen.set(r.key, r.tick);
			for (const cr of rows) {
				const h = hKey.get((cr.tick + best.lag) + '|' + cr.key);
				if (!h) continue;
				const e = dist2(h, cr);
				d2.push(e); d3.push(dist3(h, cr));
				dyaw.push(angdiff(h.yaw, cr.yaw));
				dv.push(Math.hypot(h.vx - cr.vx, h.vy - cr.vy, h.vz - cr.vz));
				if (e > 50) big[0]++;
				if (e > 200) big[1]++;
				if (e > 1024) big[2]++;
				ages.push({ age: h.tick - (firstSeen.get(h.key) || h.tick), d2: e });
			}
			report.push({
				g, ticks: ticks.size, cmp, skipped, diffTicks, maxDiff,
				meanDiff: cmp ? sumDiff / cmp : 0,
				hTotal, cTotal, s2: stats(d2), s3: stats(d3), sy: stats(dyaw), sv: stats(dv),
				best, big, ages,
			});
		}

		// Count reconciliation first: it is what "same objects?" means.
		report.sort((a, b) => b.diffTicks - a.diffTicks || b.maxDiff - a.maxDiff);
		console.log('\n  OBJECT COUNT per label (comparable ticks only; ticks the client');
		console.log('  never received are skipped, not counted as zero)');
		console.log('  label                 cmp  skipped  count-diff  max |d|  mean |d|   host obj-ticks  client obj-ticks');
		for (const r of report) {
			console.log('  ' + r.g.padEnd(20) +
				String(r.cmp).padStart(5) + String(r.skipped).padStart(9) +
				String(r.diffTicks).padStart(12) +
				String(r.maxDiff).padStart(9) +
				'  ' + f(r.meanDiff, 7) +
				String(r.hTotal).padStart(16) + String(r.cTotal).padStart(16));
		}

		// Then pose, worst first.
		const worst = [...report].sort((a, b) => b.s2.mean - a.s2.mean);
		console.log('\n  POSE residual per label, at the label\'s best lag');
		console.log('  label                 n     lag  xy mean  xy p50   xy p95    xy max   >50  >200  >1024  yaw max   dvel');
		for (const r of worst) {
			if (!r.s2.n) continue;
			console.log('  ' + r.g.padEnd(20) +
				String(r.s2.n).padStart(5) + String(r.best.lag).padStart(5) +
				'  ' + f(r.s2.mean) + ' ' + f(r.s2.p50) + ' ' + f(r.s2.p95) + '  ' + f(r.s2.max) +
				String(r.big[0]).padStart(5) + String(r.big[1]).padStart(6) + String(r.big[2]).padStart(7) +
				'  ' + f(r.sy.max, 6) + '  ' + f(r.sv.mean, 6));
		}

		// Error against age for the worst label: constant offset (replication
		// latency / wrong start) versus growing (real drift).
		if (worst.length && worst[0].s2.n) {
			const r = worst[0];
			console.log('\n  ' + r.g + ': xy error vs age (ticks since first seen), at lag ' + r.best.lag);
			const buckets = new Map();
			for (const { age, d2 } of r.ages) {
				const k = Math.floor(age / 5) * 5;
				if (!buckets.has(k)) buckets.set(k, []);
				buckets.get(k).push(d2);
			}
			for (const k of [...buckets.keys()].sort((a, b) => a - b)) {
				const s = stats(buckets.get(k));
				if (s.n < 20) continue;
				console.log('    age ' + String(k).padStart(3) + '-' + String(k + 4).padStart(3) +
					'  n ' + String(s.n).padStart(5) +
					'  mean ' + f(s.mean) + '  p95 ' + f(s.p95) + '  max ' + f(s.max));
			}
		}
	} else {
		console.log('\n== E: replicated bodies == (no E rows)');
	}
}

main();
