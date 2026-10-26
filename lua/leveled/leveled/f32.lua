-- Arredonda um intermediário como o float do C; devolve apenas o valor.
return function(value)
    return (string.unpack("f", string.pack("f", value)))
end
