-- ai_undotree.lua — undo tree viewer that tags external (AI) edits
--
-- When a buffer is reloaded because the file changed on disk (i.e. Claude or
-- any external tool wrote it), the resulting undo state is tagged [AI].
-- <leader>u opens a panel of the undo tree; <CR> jumps to a state, q closes.

local M = {}

M.ai_seqs = {} -- bufnr -> { [seq] = os.time() of the external write }
M.panel = { buf = nil, win = nil, target_buf = nil, target_win = nil, seqs = {} }

--------------------------------------------------------------------------------
-- Live reload: poll for on-disk changes so external edits show up immediately
--------------------------------------------------------------------------------
vim.o.autoread = true
local poll = vim.uv.new_timer()
poll:start(500, 500, vim.schedule_wrap(function()
  -- checktime is a no-op for modified buffers other than warning on conflict
  if vim.fn.mode() == "n" and vim.fn.getcmdwintype() == "" then
    vim.cmd("silent! checktime")
  end
end))

--------------------------------------------------------------------------------
-- Tag undo states created by external (AI) writes
--------------------------------------------------------------------------------
vim.api.nvim_create_autocmd("FileChangedShellPost", {
  group = vim.api.nvim_create_augroup("AiUndotreeTag", { clear = true }),
  callback = function(ev)
    local buf = ev.buf
    vim.schedule(function()
      if not vim.api.nvim_buf_is_valid(buf) then return end
      local seq = vim.api.nvim_buf_call(buf, function()
        return vim.fn.undotree().seq_cur
      end)
      M.ai_seqs[buf] = M.ai_seqs[buf] or {}
      M.ai_seqs[buf][seq] = os.time()
      if M.panel.win and vim.api.nvim_win_is_valid(M.panel.win)
          and M.panel.target_buf == buf then
        M.render()
      end
    end)
  end,
})

--------------------------------------------------------------------------------
-- Panel
--------------------------------------------------------------------------------
local function time_ago(t)
  local d = os.time() - t
  if d < 60 then return d .. "s ago" end
  if d < 3600 then return math.floor(d / 60) .. "m ago" end
  return math.floor(d / 3600) .. "h ago"
end

local function flatten(entries, depth, out)
  for _, e in ipairs(entries) do
    if e.alt then flatten(e.alt, depth + 1, out) end
    table.insert(out, { seq = e.seq, time = e.time, save = e.save, depth = depth })
  end
end

function M.render()
  local p = M.panel
  if not (p.buf and vim.api.nvim_buf_is_valid(p.buf)) then return end
  local ut = vim.api.nvim_buf_call(p.target_buf, vim.fn.undotree)
  local ai = M.ai_seqs[p.target_buf] or {}

  local states = {}
  flatten(ut.entries, 0, states)
  table.insert(states, 1, { seq = 0, time = 0, depth = 0 }) -- root (original file)

  local lines, seqs, ai_lines = {}, {}, {}
  local name = vim.fn.fnamemodify(vim.api.nvim_buf_get_name(p.target_buf), ":t")
  table.insert(lines, " undo tree: " .. name .. "  (CR jump / q quit)")
  table.insert(seqs, false) -- header has no state; nil would collapse the table

  for i = #states, 1, -1 do -- newest first
    local s = states[i]
    local cur = (s.seq == ut.seq_cur) and "●" or " "
    local indent = string.rep("  ", s.depth)
    local when = (s.time > 0) and time_ago(s.time) or "original"
    local tag = ai[s.seq] and "  [AI]" or ""
    local saved = s.save and " *" or ""
    table.insert(lines, string.format(" %s %s%-4d %s%s%s", cur, indent, s.seq, when, tag, saved))
    table.insert(seqs, s.seq)
    if ai[s.seq] then table.insert(ai_lines, #lines - 1) end
  end

  vim.bo[p.buf].modifiable = true
  vim.api.nvim_buf_set_lines(p.buf, 0, -1, false, lines)
  vim.bo[p.buf].modifiable = false
  p.seqs = seqs

  local ns = vim.api.nvim_create_namespace("ai_undotree")
  vim.api.nvim_buf_clear_namespace(p.buf, ns, 0, -1)
  for _, l in ipairs(ai_lines) do
    vim.hl.range(p.buf, ns, "DiagnosticVirtualTextInfo", { l, 0 }, { l, -1 })
  end
end

function M.close()
  local p = M.panel
  if p.win and vim.api.nvim_win_is_valid(p.win) then
    vim.api.nvim_win_close(p.win, true)
  end
  p.win, p.buf = nil, nil
end

function M.toggle()
  local p = M.panel
  if p.win and vim.api.nvim_win_is_valid(p.win) then
    M.close()
    return
  end
  p.target_buf = vim.api.nvim_get_current_buf()
  p.target_win = vim.api.nvim_get_current_win()

  vim.cmd("topleft 40vsplit")
  p.win = vim.api.nvim_get_current_win()
  p.buf = vim.api.nvim_create_buf(false, true)
  vim.api.nvim_win_set_buf(p.win, p.buf)
  vim.bo[p.buf].bufhidden = "wipe"
  vim.wo[p.win].number = false
  vim.wo[p.win].relativenumber = false
  vim.wo[p.win].winfixwidth = true

  vim.keymap.set("n", "q", M.close, { buffer = p.buf })
  vim.keymap.set("n", "<CR>", function()
    local seq = p.seqs[vim.api.nvim_win_get_cursor(p.win)[1]]
    if seq == nil then return end
    vim.api.nvim_win_call(p.target_win, function()
      vim.cmd("undo " .. seq)
    end)
    M.render()
  end, { buffer = p.buf })

  -- keep the panel current while the target buffer changes
  vim.api.nvim_create_autocmd({ "TextChanged", "TextChangedI" }, {
    buffer = p.target_buf,
    callback = function()
      if p.win and vim.api.nvim_win_is_valid(p.win) then M.render()
      else return true end
    end,
  })

  M.render()
end

vim.keymap.set("n", "<leader>u", M.toggle, { desc = "Toggle AI undo tree" })

return M
