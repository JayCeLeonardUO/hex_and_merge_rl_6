-- Project-local Neovim config (loaded via :h exrc)
-- Mirrors the tmux workflow from my_raylib_games:
--   <leader>R — build + run the game in a detached tmux session (popup shows build output)
--   <C-t>     — toggle a bottom pane attached to the running game session

local project_root = vim.fn.fnamemodify(vim.fn.findfile("CMakeLists.txt", ".;"), ":p:h")
local game_session = "hex-game-run"

-- Build and run in tmux session (leader+R)
vim.keymap.set("n", "<leader>R", function()
  local tmux_cmd = string.format(
    "tmux display-popup -w 80%% -h 50%% 'cd %s && ./build_run.sh; exec $SHELL'",
    project_root
  )
  vim.fn.system(tmux_cmd)
end, { desc = "Build and run game in tmux session" })

-- Toggle bottom pane showing game session (Ctrl+T)
vim.keymap.set("n", "<C-t>", function()
  local panes = vim.fn.system("tmux list-panes -F '#{pane_id}:#{pane_current_command}'")
  if panes:match("tmux") then
    vim.fn.system("tmux kill-pane -t {bottom}")
  else
    local exists = vim.fn.system("tmux has-session -t " .. game_session .. " 2>/dev/null && echo yes || echo no")
    if exists:match("yes") then
      vim.fn.system("tmux split-window -v -l 30% 'tmux attach -t " .. game_session .. "'")
    else
      vim.notify("No game session running")
    end
  end
end, { desc = "Toggle game terminal pane" })

-- Debug with gdbgui (leader+r)
vim.keymap.set("n", "<leader>r", function()
  local cmd = string.format("cd %s && ./build_run.sh gdbgui 2>/dev/null", project_root)
  vim.fn.jobstart(cmd, {
    stdout_buffered = true,
    on_exit = function(_, code)
      vim.schedule(function()
        if code == 0 then
          vim.cmd("echo 'gdbgui: http://127.0.0.1:5000'")
        else
          vim.cmd("echohl ErrorMsg | echo 'Build failed!' | echohl None")
        end
      end)
    end,
  })
end, { desc = "Build and debug with gdbgui" })

-- Auto-reload files changed externally
vim.o.autoread = true
vim.api.nvim_create_autocmd({ "FocusGained", "BufEnter", "CursorHold" }, {
  callback = function() vim.cmd("checktime") end,
})

-- Auto-reload this config when saved
vim.api.nvim_create_autocmd("BufWritePost", {
  pattern = "*/.nvim.lua",
  callback = function()
    dofile(vim.fn.expand("%:p"))
    vim.notify(".nvim.lua reloaded")
  end,
})
