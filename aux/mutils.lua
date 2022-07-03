
local M = {}
M._VERSION = 2
M._DESCRIPTION = "Common functions used across repos, mainly for testing/debugging."

function M.addtrailing(str, char)
    if str and str[#str] ~= char then
        return str .. char
    end
end

function M.striptrailing(str, char)
    if str:sub(-1,-1) == char then
        str = str:sub(1,-2)
    end
    return str
end

function M.ptable(table_obj, offset, prel)
    -- everything gets concatenated with this, which is the final result ultimately returned
    local res=""

    --prelude before the table proper
    if prel then
        prel = M.addtrailing(prel, "\n")
        res = res .. prel
    end
    res = res .. "{\n"

    -- closure
    function tts(table_obj, off)

        -- each item in the array is either a table, in which case recurse
        -- or a scakar that can be deal with on the spot without recursion
        for k,v in pairs(table_obj) do
            local curr_lvl_str = ""
           -- enclose key and value in quotation marks if they're strings to make
            -- the output more intuitive
            val = tostring(v)
            key = tostring(k)

            if type(v) == "string" then
               val = string.format("\"%s\"", val)
            end

            if type(k) == "string" then
               key = string.format("\"%s\"", key)
            end

            -- item is table: print or recurse; traverse table
            -- __index must be skipped as it often points to the same table or a metatable
            -- so it's very likely to cause an infinite loop!--> do NOT recurse on __index if a table
            if type(v) == "table" and k ~= "__index" then -- recurse
                res = res .. string.rep(" ", off)
                res = res .. "[" .. tostring(key) .. "]" .. " : { " .. "\n"  -- [<key>]
                tts(v, off+offset) -- recurse and indent appropriately
                res = res ..  string.rep(" ", off)
                res = res .. "},\n"

            else -- item is scalar
                res = res .. string.rep(" ", off)
                -- surround the value with quotation marks if it's a string,
                -- so as to make it more explicit
                res = res .. "[" .. tostring(key) .. "]" .. " : " .. tostring(val) .. ",\n"  -- [<key] : <value>
            end
        end

        if not (res:sub(-2,-1) == ",\n") then return end -- assume empty table
        -- else any recursive call ends here; we need to remove the trailing comma
        -- for the last item in any table
        res = string.sub(res, 1, #res-2)
        res = M.addtrailing(res, "\n")
    end

    -- table to string;
    tts(table_obj, offset)

    -- end
    res = res .. "}\n"

    return res
end

-- only print to stdout if DEBUG_MODE is set in the environment
function M.reveal(...)
    if os.getenv("DEBUG_MODE") then
        print(string.format(...))
    end
end

-- return output of cmd and strip any trailing newline
function M.capture(cmd)
  local handle = assert(io.popen(cmd, 'r'))
  local output = assert(handle:read('*a'))
  handle:close()

  return M.striptrailing(output, "\n")
end


return M
