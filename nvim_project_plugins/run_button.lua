-- A clickable "▶ build & run" button pinned to the editor's top-right: a
-- one-line floating window over everything. Clicking it focuses the float,
-- and the BufEnter handler immediately hops back and fires the runner -- so
-- any click anywhere on the chip acts as a button press (needs 'mouse' on;
-- <leader>R stays the keyboard path to the same runner).

local M = {}

function M.setup(opts)
  if #vim.api.nvim_list_uis() == 0 then return end -- headless: no screen, no button

  -- The .nvim.lua auto-reload re-runs setup on every save: keep the one button
  if vim.g.hex_run_button_win and vim.api.nvim_win_is_valid(vim.g.hex_run_button_win) then return end

  local label = " ▶ build & run "
  local buf = vim.api.nvim_create_buf(false, true)
  vim.api.nvim_buf_set_lines(buf, 0, -1, false, { label })
  vim.bo[buf].modifiable = false
  vim.bo[buf].bufhidden = "hide"

  vim.api.nvim_set_hl(0, "HexRunButton", { fg = "#1a1420", bg = "#ffdc3c", bold = true })

  local win = vim.api.nvim_open_win(buf, false, {
    relative = "editor",
    anchor = "NE",
    row = 0,
    col = vim.o.columns,
    width = vim.fn.strdisplaywidth(label),
    height = 1,
    style = "minimal",
    zindex = 250,
    focusable = true,
  })
  vim.wo[win].winhl = "Normal:HexRunButton"
  vim.g.hex_run_button_win = win

  -- Stay glued to the corner when the terminal resizes
  vim.api.nvim_create_autocmd("VimResized", {
    callback = function()
      if vim.api.nvim_win_is_valid(win) then
        vim.api.nvim_win_set_config(win, { relative = "editor", anchor = "NE", row = 0, col = vim.o.columns })
      end
    end,
  })

  -- The press: entering the button's buffer (a mouse click is the only way
  -- in) hops straight back to where you were and runs
  vim.api.nvim_create_autocmd("BufEnter", {
    buffer = buf,
    callback = function()
      vim.schedule(function()
        vim.cmd.wincmd("p")
        opts.run()
      end)
    end,
  })
end

return M
