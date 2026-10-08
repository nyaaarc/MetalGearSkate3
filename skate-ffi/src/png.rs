//! Minimal PNG decoder for the board textures the converter embeds in `skater.glb`: 8-bit,
//! non-interlaced, greyscale/RGB/RGBA (with or without alpha), decoded to RGBA8.

/// Decodes `data` to (width, height, RGBA8 rows top to bottom).
pub fn decode(data: &[u8]) -> Result<(u32, u32, Vec<u8>), String> {
    if data.get(..8) != Some(b"\x89PNG\r\n\x1a\n".as_slice()) {
        return Err("not a PNG".into());
    }
    let (mut width, mut height, mut channels) = (0u32, 0u32, 0usize);
    let mut idat = Vec::new();
    let mut at = 8;
    while at + 8 <= data.len() {
        let len = u32::from_be_bytes(data[at..at + 4].try_into().unwrap()) as usize;
        let kind = &data[at + 4..at + 8];
        let body = data.get(at + 8..at + 8 + len).ok_or("truncated PNG chunk")?;
        match kind {
            b"IHDR" => {
                if body.len() < 13 {
                    return Err("short IHDR".into());
                }
                width = u32::from_be_bytes(body[0..4].try_into().unwrap());
                height = u32::from_be_bytes(body[4..8].try_into().unwrap());
                let (depth, colour, interlace) = (body[8], body[9], body[12]);
                channels = match colour {
                    0 => 1,
                    2 => 3,
                    4 => 2,
                    6 => 4,
                    other => return Err(format!("unsupported PNG colour type {other}")),
                };
                if depth != 8 || interlace != 0 {
                    return Err(format!("unsupported PNG (bit depth {depth}, interlace {interlace})"));
                }
            }
            b"IDAT" => idat.extend_from_slice(body),
            b"IEND" => break,
            _ => {}
        }
        at += 12 + len;
    }
    if width == 0 || height == 0 || channels == 0 {
        return Err("PNG has no image header".into());
    }
    let raw = miniz_oxide::inflate::decompress_to_vec_zlib(&idat).map_err(|e| format!("PNG inflate: {e:?}"))?;
    let stride = width as usize * channels;
    if raw.len() < (stride + 1) * height as usize {
        return Err("PNG image data too short".into());
    }
    let mut pixels = vec![0u8; stride * height as usize];
    for y in 0..height as usize {
        let filter = raw[y * (stride + 1)];
        let line = &raw[y * (stride + 1) + 1..(y + 1) * (stride + 1)];
        let (done, rest) = pixels.split_at_mut(y * stride);
        let prev = if y > 0 { &done[(y - 1) * stride..] } else { &[][..] };
        let out = &mut rest[..stride];
        for x in 0..stride {
            let a = if x >= channels { out[x - channels] } else { 0 };
            let b = if y > 0 { prev[x] } else { 0 };
            let c = if y > 0 && x >= channels { prev[x - channels] } else { 0 };
            out[x] = line[x].wrapping_add(match filter {
                0 => 0,
                1 => a,
                2 => b,
                3 => ((u16::from(a) + u16::from(b)) / 2) as u8,
                4 => paeth(a, b, c),
                other => return Err(format!("bad PNG filter {other}")),
            });
        }
    }
    let rgba = match channels {
        4 => pixels,
        3 => pixels.chunks_exact(3).flat_map(|p| [p[0], p[1], p[2], 255]).collect(),
        2 => pixels.chunks_exact(2).flat_map(|p| [p[0], p[0], p[0], p[1]]).collect(),
        _ => pixels.iter().flat_map(|&g| [g, g, g, 255]).collect(),
    };
    Ok((width, height, rgba))
}

fn paeth(a: u8, b: u8, c: u8) -> u8 {
    let p = i16::from(a) + i16::from(b) - i16::from(c);
    let (pa, pb, pc) = ((p - i16::from(a)).abs(), (p - i16::from(b)).abs(), (p - i16::from(c)).abs());
    if pa <= pb && pa <= pc {
        a
    } else if pb <= pc {
        b
    } else {
        c
    }
}
