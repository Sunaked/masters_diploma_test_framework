-- ============================================================================
--
-- ============================================================================

global = {
	target_total_time_minutes = 15,
	base_runs_per_config = 10,
	min_runs_per_config = 10,
	metrics_sampling_ms = 100,
	pin_threads = true,
	use_rdtsc_for_latency = true,

	-- Overhead decomposition
	latency_off = false, -- true = skip latency recording (EmptyExecutor mode)
	sampler_off = false, -- true = skip SystemSampler thread

	-- Dual latency
	-- "cycles" (rdtsc only), "wall_ns" (steady_clock only), "both"
	latency_mode = "both",

	-- Reservoir sampling
	reservoir_capacity = 1000000, -- samples per thread (default 1M, ~8MB)

	-- Warmup auto-duration
	warmup_auto_seconds = 0.0, -- 0 = warmup phases run their declared ops once
}

threads_list = { 1, 4 }

containers = {
	{
		name = "MyOwnContainer",
		type = "custom",
		target_time_minutes = 5,
	},

	-- NullContainer for overhead measurement:
	{
		name = "NullBaseline",
		type = "null",
		target_time_minutes = 5,
	},
}

-- ============================================================================
-- Lua generator functions для ScriptedStep фаз.
-- ============================================================================

function alternating_hotspot(ctx, i)
	if i % 10 == 0 then
		return { op = "erase", key = i % 1000 }
	elseif i % 2 == 0 then
		return { op = "find", key = 42 }
	else
		return { op = "insert", key = i + ctx.thread_id * ctx.ops_per_thread }
	end
end

function scan_then_read(ctx, i)
	local switch_point = math.floor(ctx.ops_per_thread * 0.7)
	local base_key = ctx.thread_id * ctx.ops_per_thread

	if i <= switch_point then
		return { op = "insert", key = base_key + i }
	else
		local read_key = base_key + ((i - switch_point) % switch_point) + 1
		return { op = "find", key = read_key }
	end
end

function burst_pattern(ctx, i)
	local cycle = i % 160
	if cycle < 100 then
		return "insert", i + ctx.thread_id * 1000000
	elseif cycle < 150 then
		return "find", (i % 10000)
	else
		return "erase", (i % 5000)
	end
end

-- ============================================================================
-- Сценарии
-- ============================================================================

scenarios = {
	-- ====================================================================
	-- Multi-phase mixed scenario with explicit phase roles [Req #2]
	-- ====================================================================
	{
		name = "multi_phase_mixed",
		phases = {
			-- Phase 1: prefill via ScriptedPlan — OUTSIDE timing
			{
				mode = "scripted_plan",
				role = "prefill", -- runs before timing starts
				plan = {
					{ count = 400000, op = "insert", key_mode = "sequential", start = 0 },
				},
			},
			-- Phase 2: warmup — timed but stats discarded
			{
				mode = "probabilistic",
				role = "warmup", -- stats not recorded
				ops_per_thread = 500000,
				insert = 0.05,
				find = 0.90,
				erase = 0.05,
				key_dist = "zipfian",
				alpha = 0.99,
				key_range = 100000,
			},
			-- Phase 3: measured read-heavy probabilistic
			{
				mode = "probabilistic",
				role = "measure", -- this goes into raw_results.csv
				ops_per_thread = 800000,
				insert = 0.05,
				find = 0.90,
				erase = 0.05,
				key_dist = "zipfian",
				alpha = 0.99,
				key_range = 100000,
			},
			-- Phase 4: measured scripted cleanup
			{
				mode = "scripted_step",
				role = "measure",
				ops_per_thread = 200000,
				script = "alternating_hotspot",
				key_range = 100000,
			},
		},
	},
}
