-- Dev aid (gmod-dev only): run console commands written to data/devcmd.txt,
-- since this engine branch has no -netconport. Output: data/devcmd_out.txt.
timer.Create("devcmd", 0.25, 0, function()
  local text = file.Read("devcmd.txt", "DATA")
  if not text or #text == 0 then return end
  file.Delete("devcmd.txt")
  for line in string.gmatch(text, "[^\r\n]+") do
    if line:sub(1, 4) == "lua " then
      local ok, err = pcall(CompileString(line:sub(5), "devcmd"))
      file.Append("devcmd_out.txt", tostring(ok) .. " " .. tostring(err) .. "\n")
    else
      LocalPlayer():ConCommand(line)
    end
  end
end)
