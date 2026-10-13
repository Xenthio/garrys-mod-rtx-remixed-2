-- Dev aid: a scripted play session for frame-time measurements.
-- Install as garrysmod/lua/autorun/openremix_bench.lua (single player).
--
--   openremix_bench        runs the session (about 75 s)
--   openremix_bench_stop   ends it early
--
-- The session is the same every run: the player is put at the map's first
-- spawn point, the map is cleaned up, and everything that happens is a
-- function of the server tick since the start (no clock, no math.random). It
-- walks, turns, spawns props and ragdolls, shoots, sets off explosions, places
-- lights and NPCs and removes it all again: the things that cause frame-time
-- spikes in normal play. Physics and NPCs are not bit-exact from run to run,
-- but what is asked of the game, and when, is.
--
-- The client records every frame's time with the phase it fell in and writes
--   data/openremix_bench_frames.txt   "<phase index> <milliseconds>" per frame
--   data/openremix_bench_result.txt   per-phase and overall statistics
-- when the session ends; data/openremix_bench_state.txt holds "running" or
-- "done <run number>" for whoever waits for it.

local TAG = "openremix_bench"

-- The session: phases in order, seconds each. Movement is in units a second
-- (forward, right), turning in degrees a second.
local PHASES = {
  { name = "settle",       seconds = 3 },
  { name = "walk",         seconds = 8,  forward = 200, yaw = 18, pitch = 0 },
  { name = "spawn props",  seconds = 7,  forward = 0,   yaw = 0,  pitch = 8,  props = 5 },      -- props a second
  { name = "shoot props",  seconds = 8,  right = 120,   yaw = -12, pitch = 6, attack = true },
  { name = "explosions",   seconds = 6,  forward = -80, yaw = 0,  pitch = 6,  explosions = 1.5 },
  { name = "ragdolls",     seconds = 5,  yaw = 25, pitch = 4, ragdolls = 2 },
  { name = "lights",       seconds = 6,  forward = 150, yaw = 30, pitch = 0, lights = 1, flashlight = true },
  { name = "npcs",         seconds = 7,  yaw = -20, pitch = 2, npcs = 1, attack = true },
  { name = "fast turns",   seconds = 8,  turn = 220 },                                          -- back and forth
  { name = "run",          seconds = 7,  forward = 400, yaw = -25, pitch = -4, sprint = true },
  { name = "remove all",   seconds = 5,  yaw = 10, remove = true },
  { name = "settle after", seconds = 5 },
}

local PROP_MODELS = {
  "models/props_c17/oildrum001.mdl",
  "models/props_junk/wood_crate001a.mdl",
  "models/props_c17/FurnitureCouch001a.mdl",
  "models/props_junk/PopCan01a.mdl",
  "models/props_c17/chair02a.mdl",
  "models/props_junk/wood_pallet001a.mdl",
  "models/props_wasteland/controlroom_filecabinet002a.mdl",
  "models/props_junk/TrafficCone001a.mdl",
  "models/props_interiors/Furniture_Couch02a.mdl",
  "models/props_c17/oildrum001_explosive.mdl",
}
local RAGDOLL_MODELS = {
  "models/Humans/Group01/male_07.mdl",
  "models/Humans/Group01/female_02.mdl",
  "models/Kleiner.mdl",
}
local NPC_CLASSES = { "npc_citizen", "npc_kleiner", "npc_citizen", "npc_eli" }

local function phaseAt(seconds)
  local start = 0
  for index, phase in ipairs(PHASES) do
    if seconds < start + phase.seconds then return index, phase, seconds - start end
    start = start + phase.seconds
  end
  return nil
end

if SERVER then
  local run = nil -- the session in progress
  local runs = 0

  -- A sequence of numbers in [0, 1) that is the same every run.
  local function sequence(seed)
    local state = seed
    return function()
      state = (state * 1103515245 + 12345) % 2147483648
      return state / 2147483648
    end
  end

  local function track(entity)
    if IsValid(entity) then run.entities[#run.entities + 1] = entity end
    return entity
  end

  -- A point in front of where the current phase began, in its frame: forward, right, up.
  local function place(forward, right, up)
    return run.origin + run.forward * forward + run.right * right + Vector(0, 0, up)
  end

  local function spawnProp(index)
    local random = run.random
    local prop = ents.Create("prop_physics")
    if not IsValid(prop) then return end
    prop:SetModel(PROP_MODELS[(index - 1) % #PROP_MODELS + 1])
    -- A grid ahead, dropped from a little above the ground.
    local column, row = (index - 1) % 6, math.floor((index - 1) / 6)
    prop:SetPos(place(260 + row * 70, (column - 2.5) * 80, 60 + random() * 80))
    prop:SetAngles(Angle(random() * 40 - 20, random() * 360, random() * 40 - 20))
    prop:Spawn()
    prop:Activate()
    track(prop)
  end

  local function spawnRagdoll(index)
    local ragdoll = ents.Create("prop_ragdoll")
    if not IsValid(ragdoll) then return end
    ragdoll:SetModel(RAGDOLL_MODELS[(index - 1) % #RAGDOLL_MODELS + 1])
    ragdoll:SetPos(place(220 + index * 30, (index % 5 - 2) * 70, 140))
    ragdoll:Spawn()
    ragdoll:Activate()
    track(ragdoll)
  end

  local function explode(index)
    local explosion = ents.Create("env_explosion")
    if not IsValid(explosion) then return end
    explosion:SetPos(place(300 + (index % 4) * 80, (index % 3 - 1) * 120, 24))
    explosion:SetKeyValue("iMagnitude", "80")
    explosion:Spawn()
    explosion:Fire("Explode", "", 0)
    explosion:Fire("Kill", "", 2)
  end

  local function spawnLight(index)
    local light = ents.Create("light_dynamic")
    if not IsValid(light) then return end
    local colours = { "255 180 120 255", "120 180 255 255", "160 255 160 255" }
    light:SetPos(place(200 + index * 90, (index % 3 - 1) * 160, 90))
    light:SetKeyValue("_light", colours[(index - 1) % #colours + 1])
    light:SetKeyValue("brightness", "4")
    light:SetKeyValue("distance", "400")
    light:SetKeyValue("style", "0")
    light:Spawn()
    light:Activate()
    light:Fire("TurnOn", "", 0)
    track(light)
  end

  local function spawnNpc(index)
    local npc = ents.Create(NPC_CLASSES[(index - 1) % #NPC_CLASSES + 1])
    if not IsValid(npc) then return end
    npc:SetPos(place(280 + (index % 3) * 60, (index % 4 - 1.5) * 110, 8))
    npc:SetAngles(Angle(0, (run.yaw + 180) % 360, 0))
    npc:Spawn()
    npc:Activate()
    track(npc)
  end

  local function finish()
    if not run then return end
    for _, entity in ipairs(run.entities) do
      if IsValid(entity) then entity:Remove() end
    end
    local ply = run.player
    if IsValid(ply) then
      ply:Flashlight(false)
      ply:GodDisable()
    end
    hook.Remove("Tick", TAG)
    hook.Remove("StartCommand", TAG)
    run = nil
    SetGlobalInt(TAG .. "_phase", 0)
    print("[openremix_bench] session " .. runs .. " ended")
  end

  -- What a phase spawns: `rate` things a second, each exactly once.
  local function due(counter, rate, elapsed)
    local wanted = math.floor(elapsed * rate) + 1
    local have = run.count[counter] or 0
    if have >= wanted then return nil end
    run.count[counter] = have + 1
    return have + 1
  end

  local function tick()
    if not run then return end
    local ply = run.player
    if not IsValid(ply) then finish() return end
    local seconds = (engine.TickCount() - run.tick) * engine.TickInterval()
    local index, phase, elapsed = phaseAt(seconds)
    if not index then finish() return end
    if index ~= run.phase then
      run.phase = index
      run.count = {}
      SetGlobalInt(TAG .. "_phase", index)
      if phase.flashlight then ply:Flashlight(true) end
      -- What a phase spawns is placed in front of where the player stands
      -- and looks when it begins.
      local facing = Angle(0, run.yaw % 360, 0)
      run.origin, run.forward, run.right = ply:GetPos(), facing:Forward(), facing:Right()
    end
    -- The view: every phase turns from where the one before left off.
    local interval = engine.TickInterval()
    if phase.turn then
      -- Left for a second, right for a second, and so on.
      local direction = math.floor(elapsed) % 2 == 0 and 1 or -1
      run.yaw = run.yaw + direction * phase.turn * interval
    elseif phase.yaw then
      run.yaw = run.yaw + phase.yaw * interval
    end
    local pitch = phase.pitch or 0
    run.pitch = run.pitch + math.Clamp(pitch - run.pitch, -40 * interval, 40 * interval)
    ply:SetEyeAngles(Angle(run.pitch, run.yaw % 360, 0))

    local n = phase.props and due("props", phase.props, elapsed)
    if n then
      run.props = run.props + 1
      spawnProp(run.props)
    end
    n = phase.ragdolls and due("ragdolls", phase.ragdolls, elapsed)
    if n then spawnRagdoll(n) end
    n = phase.explosions and due("explosions", phase.explosions, elapsed)
    if n then explode(n) end
    n = phase.lights and due("lights", phase.lights, elapsed)
    if n then spawnLight(n) end
    n = phase.npcs and due("npcs", phase.npcs, elapsed)
    if n then spawnNpc(n) end
    if phase.remove then
      -- Everything goes over the phase, oldest first.
      local total = #run.entities
      local upTo = math.min(total, math.ceil(total * (elapsed + interval) / phase.seconds))
      for i = run.removed + 1, upTo do
        if IsValid(run.entities[i]) then run.entities[i]:Remove() end
      end
      run.removed = math.max(run.removed, upTo)
    end
  end

  local function command(ply, cmd)
    if not run or ply ~= run.player then return end
    local _, phase = phaseAt((engine.TickCount() - run.tick) * engine.TickInterval())
    cmd:ClearMovement()
    cmd:ClearButtons()
    if not phase then return end
    cmd:SetViewAngles(Angle(run.pitch, run.yaw % 360, 0))
    cmd:SetForwardMove(phase.forward or 0)
    cmd:SetSideMove(phase.right or 0)
    local buttons = 0
    if phase.attack then buttons = bit.bor(buttons, IN_ATTACK) end
    if phase.sprint then buttons = bit.bor(buttons, IN_SPEED) end
    cmd:SetButtons(buttons)
  end

  local function start(ply)
    if run then finish() end
    if not IsValid(ply) then ply = player.GetAll()[1] end
    if not IsValid(ply) then print("[openremix_bench] no player") return end
    game.CleanUpMap()
    -- The map's first spawn point, by entity index: the same one every run.
    local origin, yaw = ply:GetPos(), ply:EyeAngles().yaw
    local spawns = ents.FindByClass("info_player_start")
    table.sort(spawns, function(a, b) return a:EntIndex() < b:EntIndex() end)
    if IsValid(spawns[1]) then
      origin, yaw = spawns[1]:GetPos(), spawns[1]:GetAngles().yaw
    end
    ply:SetPos(origin)
    ply:SetVelocity(-ply:GetVelocity())
    ply:SetEyeAngles(Angle(0, yaw, 0))
    ply:GodEnable()
    ply:AllowFlashlight(true)
    ply:Flashlight(false)
    ply:StripWeapons()
    ply:Give("weapon_smg1")
    ply:GiveAmmo(9999, "SMG1", true)
    ply:SelectWeapon("weapon_smg1")
    runs = runs + 1
    local facing = Angle(0, yaw, 0)
    run = {
      player = ply, tick = engine.TickCount(), phase = 0, count = {}, entities = {}, removed = 0, props = 0,
      origin = origin, forward = facing:Forward(), right = facing:Right(), yaw = yaw, pitch = 0,
      random = sequence(20261012),
    }
    SetGlobalInt(TAG .. "_run", runs)
    hook.Add("Tick", TAG, tick)
    hook.Add("StartCommand", TAG, command)
    print("[openremix_bench] session " .. runs .. " started")
  end

  concommand.Add("openremix_bench", function(ply) start(ply) end)
  concommand.Add("openremix_bench_stop", function() finish() end)
end

if CLIENT then
  local frames, phases, last, recording, runNumber = {}, {}, nil, false, 0

  local function statistics(values)
    local sorted = table.Copy(values)
    table.sort(sorted)
    local count = #sorted
    if count == 0 then return nil end
    local sum, change = 0, 0
    for i = 1, count do sum = sum + values[i] end
    for i = 2, count do change = change + math.abs(values[i] - values[i - 1]) end
    local mean = sum / count
    local over = 0
    for i = 1, count do if values[i] > mean * 1.5 then over = over + 1 end end
    local function at(q) return sorted[math.min(count, math.floor(q * count) + 1)] end
    return {
      count = count, mean = mean, median = at(0.5), p95 = at(0.95), p99 = at(0.99), max = sorted[count],
      change = count > 1 and change / (count - 1) or 0, over = over,
    }
  end

  local function line(name, s)
    return string.format("%-14s frames %5d | mean %6.2f ms (%5.1f fps) median %6.2f  95%% %6.2f  99%% %6.2f  max %7.2f"
      .. " | frame-to-frame %5.2f | over 1.5x mean %d", name, s.count, s.mean, 1000 / s.mean, s.median, s.p95, s.p99,
      s.max, s.change, s.over)
  end

  local function write()
    local out, all, raw = {}, {}, {}
    for index, phase in ipairs(PHASES) do
      local values = {}
      for i = 1, #frames do
        if phases[i] == index then values[#values + 1] = frames[i] end
      end
      local s = statistics(values)
      if s then out[#out + 1] = line(phase.name, s) end
    end
    for i = 1, #frames do
      all[i] = frames[i]
      raw[i] = phases[i] .. " " .. string.format("%.3f", frames[i])
    end
    local s = statistics(all)
    if s then table.insert(out, 1, line("whole session", s)) end
    table.insert(out, 1, "openremix_bench run " .. runNumber .. " on " .. game.GetMap())
    file.Write(TAG .. "_result.txt", table.concat(out, "\n") .. "\n")
    file.Write(TAG .. "_frames.txt", table.concat(raw, "\n") .. "\n")
    file.Write(TAG .. "_state.txt", "done " .. runNumber .. "\n")
  end

  hook.Add("Think", TAG, function()
    local phase = GetGlobalInt(TAG .. "_phase", 0)
    local now = SysTime()
    if phase > 0 then
      if not recording then
        recording, frames, phases, last = true, {}, {}, nil
        runNumber = GetGlobalInt(TAG .. "_run", 0)
        file.Write(TAG .. "_state.txt", "running " .. runNumber .. "\n")
      end
      if last then
        frames[#frames + 1] = (now - last) * 1000
        phases[#phases + 1] = phase
      end
      last = now
    elseif recording then
      recording = false
      write()
    end
  end)
end
