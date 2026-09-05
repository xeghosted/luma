-- The same thing scripts/hello.lua does, written the way a resource does it.

CreateThread(function()
    while true do
        -- Slow on purpose. This is an example that runs for as long as the
        -- console is on, and its output is worth finding in the log rather
        -- than scrolling past.
        Wait(30000)
        local ped = PLAYER_PED_ID()
        local c = GET_ENTITY_COORDS(ped, 1, 0)
        print(string.format("hello: ped=%d at %.1f, %.1f, %.1f", ped, c[1], c[2], c[3]))
    end
end)

AddEventHandler('onResourceStart', function(name)
    if name == GetCurrentResourceName() then
        print('hello started')
    end
end)

exports('where', function()
    local c = GET_ENTITY_COORDS(PLAYER_PED_ID(), 1, 0)
    return c[1], c[2], c[3]
end)

RegisterCommand('where', function(source, args, raw)
    local x, y, z = exports.hello:where()
    print(string.format("you are at %.1f, %.1f, %.1f", x, y, z))
end)
