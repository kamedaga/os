#![no_std]

extern crate alloc;

use alloc::vec::Vec;

use cap_std::fs::RootDir;

const MAGIC: &[u8; 8] = b"CAPFNT1\0";
const VERSION: u32 = 1;
const RLE_VERSION: u32 = 2;
const MIN_HEADER_BYTES: usize = 64;
const RLE_HEADER_BYTES: usize = 80;
const GLYPH_RECORD_BYTES: usize = 20;
const MAX_FONT_FILE_BYTES: usize = 16 * 1024 * 1024;
const ATLAS_ENCODING_ATLAS_RLE16: u32 = 1;
const ATLAS_ENCODING_GLYPH_RLE16: u32 = 2;
const INITIAL_PREVIOUS_CODEPOINT: u32 = u32::MAX;

#[derive(Copy, Clone, Debug, Eq, PartialEq)]
pub enum Error {
    Fs(cap_std::Error),
    OutOfMemory,
    TooShort,
    BadMagic,
    UnsupportedVersion,
    UnsupportedEncoding,
    InvalidHeader,
    InvalidGlyph,
}

impl From<cap_std::Error> for Error {
    fn from(value: cap_std::Error) -> Self {
        Self::Fs(value)
    }
}

#[derive(Copy, Clone, Debug, Eq, PartialEq)]
pub struct Glyph {
    pub codepoint: u32,
    pub advance: u16,
    pub bearing_x: i16,
    pub top_offset: i16,
    pub width: u16,
    pub height: u16,
    pub atlas_x: u16,
    pub atlas_y: u16,
}

#[derive(Clone, Debug)]
pub struct LoadedFont {
    bytes: Vec<u8>,
}

impl LoadedFont {
    pub fn load_rootfs(path: &str) -> Result<Self, Error> {
        let root = RootDir::connect_default()?;
        Self::load_from_root(&root, path)
    }

    pub fn load_from_root(root: &RootDir, path: &str) -> Result<Self, Error> {
        let mut bytes = root.read_to_vec_bounded(path, MAX_FONT_FILE_BYTES)?;
        if read_u32(&bytes, 8)? == RLE_VERSION {
            bytes = expand_rle_font(&bytes)?;
        }
        Font::parse(&bytes)?;
        Ok(Self { bytes })
    }

    pub fn font(&self) -> Result<Font<'_>, Error> {
        Font::parse(&self.bytes)
    }
}

fn expand_rle_font(bytes: &[u8]) -> Result<Vec<u8>, Error> {
    if bytes.len() < RLE_HEADER_BYTES {
        return Err(Error::TooShort);
    }
    if &bytes[..8] != MAGIC {
        return Err(Error::BadMagic);
    }
    if read_u32(bytes, 8)? != RLE_VERSION {
        return Err(Error::UnsupportedVersion);
    }

    let header_size = read_u32(bytes, 12)? as usize;
    let glyph_count = read_u32(bytes, 16)? as usize;
    let glyph_record_bytes = read_u32(bytes, 20)? as usize;
    let atlas_width = read_u32(bytes, 48)? as usize;
    let atlas_height = read_u32(bytes, 52)? as usize;
    let atlas_encoding = read_u32(bytes, 64)?;
    let encoded_atlas_bytes = read_u32(bytes, 68)? as usize;
    let decoded_atlas_bytes = read_u32(bytes, 72)? as usize;

    if header_size < RLE_HEADER_BYTES
        || glyph_count == 0
        || glyph_record_bytes != GLYPH_RECORD_BYTES
    {
        return Err(Error::InvalidHeader);
    }
    if atlas_encoding != ATLAS_ENCODING_ATLAS_RLE16 && atlas_encoding != ATLAS_ENCODING_GLYPH_RLE16
    {
        return Err(Error::UnsupportedEncoding);
    }
    let expected_atlas_bytes = atlas_width
        .checked_mul(atlas_height)
        .ok_or(Error::InvalidHeader)?;
    if expected_atlas_bytes != decoded_atlas_bytes {
        return Err(Error::InvalidHeader);
    }

    let glyph_bytes = glyph_count
        .checked_mul(GLYPH_RECORD_BYTES)
        .ok_or(Error::InvalidHeader)?;
    let decoded_font_bytes = MIN_HEADER_BYTES
        .checked_add(glyph_bytes)
        .and_then(|value| value.checked_add(decoded_atlas_bytes))
        .ok_or(Error::InvalidHeader)?;
    let atlas_offset = header_size
        .checked_add(glyph_bytes)
        .ok_or(Error::InvalidHeader)?;
    let atlas_end = atlas_offset
        .checked_add(encoded_atlas_bytes)
        .ok_or(Error::InvalidHeader)?;
    if bytes.len() < atlas_end {
        return Err(Error::TooShort);
    }

    let encoded = &bytes[atlas_offset..atlas_end];
    let atlas = match atlas_encoding {
        ATLAS_ENCODING_ATLAS_RLE16 => decode_atlas_rle16(encoded, decoded_atlas_bytes)?,
        ATLAS_ENCODING_GLYPH_RLE16 => decode_glyph_rle16_atlas(
            bytes,
            header_size,
            glyph_count,
            encoded,
            atlas_width,
            atlas_height,
        )?,
        _ => return Err(Error::UnsupportedEncoding),
    };

    let mut normalized = Vec::new();
    normalized
        .try_reserve_exact(decoded_font_bytes)
        .map_err(|_| Error::OutOfMemory)?;
    normalized.extend_from_slice(&bytes[..MIN_HEADER_BYTES]);
    write_u32(&mut normalized, 8, VERSION)?;
    write_u32(&mut normalized, 12, MIN_HEADER_BYTES as u32)?;
    normalized.extend_from_slice(&bytes[header_size..header_size + glyph_bytes]);
    normalized.extend_from_slice(&atlas);
    Ok(normalized)
}

fn decode_atlas_rle16(encoded: &[u8], decoded_atlas_bytes: usize) -> Result<Vec<u8>, Error> {
    let mut atlas: Vec<u8> = Vec::new();
    atlas
        .try_reserve_exact(decoded_atlas_bytes)
        .map_err(|_| Error::OutOfMemory)?;
    atlas.resize(decoded_atlas_bytes, 0);
    let mut cursor = 0usize;
    let mut write = 0usize;
    while cursor < encoded.len() {
        let (run, value) = read_rle16_run(encoded, &mut cursor)?;
        let end = write.checked_add(run).ok_or(Error::InvalidHeader)?;
        if end > decoded_atlas_bytes {
            return Err(Error::InvalidHeader);
        }
        atlas[write..end].fill(value);
        write = end;
    }
    if write != decoded_atlas_bytes {
        return Err(Error::InvalidHeader);
    }
    Ok(atlas)
}

fn decode_glyph_rle16_atlas(
    bytes: &[u8],
    header_size: usize,
    glyph_count: usize,
    encoded: &[u8],
    atlas_width: usize,
    atlas_height: usize,
) -> Result<Vec<u8>, Error> {
    let mut atlas = Vec::new();
    let atlas_bytes = atlas_width
        .checked_mul(atlas_height)
        .ok_or(Error::InvalidHeader)?;
    atlas
        .try_reserve_exact(atlas_bytes)
        .map_err(|_| Error::OutOfMemory)?;
    atlas.resize(atlas_bytes, 0);

    let mut cursor = 0usize;
    let cell_width = read_u32(bytes, 24)?;
    let cell_height = read_u32(bytes, 28)?;
    for index in 0..glyph_count {
        let glyph = read_glyph_record(bytes, header_size + index * GLYPH_RECORD_BYTES)?;
        validate_glyph_cell(glyph, cell_width, cell_height, atlas_width, atlas_height)?;
        let glyph_width = glyph.width as usize;
        let glyph_height = glyph.height as usize;
        let glyph_bytes = glyph_width
            .checked_mul(glyph_height)
            .ok_or(Error::InvalidHeader)?;
        let dst_x = glyph.atlas_x as usize
            + clamp_i16_to_cell(glyph.bearing_x, glyph.width as u32, cell_width) as usize;
        let dst_y = glyph.atlas_y as usize
            + clamp_i16_to_cell(glyph.top_offset, glyph.height as u32, cell_height) as usize;
        let mut decoded = 0usize;
        while decoded < glyph_bytes {
            let (run, value) = read_rle16_run(encoded, &mut cursor)?;
            if decoded.saturating_add(run) > glyph_bytes {
                return Err(Error::InvalidHeader);
            }
            if value != 0 {
                write_glyph_run(
                    &mut atlas,
                    atlas_width,
                    dst_x,
                    dst_y,
                    glyph_width,
                    decoded,
                    run,
                    value,
                )?;
            }
            decoded += run;
        }
    }
    if cursor != encoded.len() {
        return Err(Error::InvalidHeader);
    }
    Ok(atlas)
}

fn write_glyph_run(
    atlas: &mut [u8],
    atlas_width: usize,
    dst_x: usize,
    dst_y: usize,
    glyph_width: usize,
    mut decoded: usize,
    mut run: usize,
    value: u8,
) -> Result<(), Error> {
    if glyph_width == 0 {
        return Err(Error::InvalidHeader);
    }
    while run > 0 {
        let x = decoded % glyph_width;
        let y = decoded / glyph_width;
        let chunk = (glyph_width - x).min(run);
        let dst = (dst_y + y)
            .checked_mul(atlas_width)
            .and_then(|row| row.checked_add(dst_x + x))
            .ok_or(Error::InvalidHeader)?;
        let dst_end = dst.checked_add(chunk).ok_or(Error::InvalidHeader)?;
        if dst_end > atlas.len() {
            return Err(Error::InvalidHeader);
        }
        atlas[dst..dst_end].fill(value);
        decoded += chunk;
        run -= chunk;
    }
    Ok(())
}

fn read_rle16_run(encoded: &[u8], cursor: &mut usize) -> Result<(usize, u8), Error> {
    if *cursor + 3 > encoded.len() {
        return Err(Error::InvalidHeader);
    }
    let run = u16::from_le_bytes([encoded[*cursor], encoded[*cursor + 1]]) as usize;
    let value = encoded[*cursor + 2];
    if run == 0 {
        return Err(Error::InvalidHeader);
    }
    *cursor += 3;
    Ok((run, value))
}

fn read_glyph_record(bytes: &[u8], offset: usize) -> Result<Glyph, Error> {
    Ok(Glyph {
        codepoint: read_u32(bytes, offset)?,
        advance: read_u16(bytes, offset + 4)?,
        bearing_x: read_i16(bytes, offset + 6)?,
        top_offset: read_i16(bytes, offset + 8)?,
        width: read_u16(bytes, offset + 10)?,
        height: read_u16(bytes, offset + 12)?,
        atlas_x: read_u16(bytes, offset + 14)?,
        atlas_y: read_u16(bytes, offset + 16)?,
    })
}

fn clamp_i16_to_cell(value: i16, glyph_size: u32, cell_size: u32) -> u32 {
    let max = cell_size.saturating_sub(glyph_size);
    if value <= 0 {
        0
    } else {
        (value as u32).min(max)
    }
}

#[derive(Copy, Clone)]
pub struct Font<'a> {
    bytes: &'a [u8],
    header_size: usize,
    glyph_count: usize,
    atlas_offset: usize,
    atlas_bytes: usize,
    cell_width: u32,
    cell_height: u32,
    cell_advance: u32,
    line_height: u32,
    atlas_width: u32,
    atlas_height: u32,
    fallback_index: usize,
}

impl<'a> Font<'a> {
    pub fn parse(bytes: &'a [u8]) -> Result<Self, Error> {
        if bytes.len() < MIN_HEADER_BYTES {
            return Err(Error::TooShort);
        }
        if &bytes[..8] != MAGIC {
            return Err(Error::BadMagic);
        }
        if read_u32(bytes, 8)? != VERSION {
            return Err(Error::UnsupportedVersion);
        }
        let header_size = read_u32(bytes, 12)? as usize;
        let glyph_count = read_u32(bytes, 16)? as usize;
        let glyph_record_bytes = read_u32(bytes, 20)? as usize;
        let cell_width = read_u32(bytes, 24)?;
        let cell_height = read_u32(bytes, 28)?;
        let cell_advance = read_u32(bytes, 32)?;
        let line_height = read_u32(bytes, 36)?;
        let atlas_width = read_u32(bytes, 48)?;
        let atlas_height = read_u32(bytes, 52)?;
        let fallback_index = read_u32(bytes, 56)? as usize;

        if header_size < MIN_HEADER_BYTES
            || glyph_count == 0
            || glyph_record_bytes != GLYPH_RECORD_BYTES
            || cell_width == 0
            || cell_height == 0
            || atlas_width == 0
            || atlas_height == 0
            || fallback_index >= glyph_count
        {
            return Err(Error::InvalidHeader);
        }

        let glyph_bytes = glyph_count
            .checked_mul(GLYPH_RECORD_BYTES)
            .ok_or(Error::InvalidHeader)?;
        let atlas_bytes = (atlas_width as usize)
            .checked_mul(atlas_height as usize)
            .ok_or(Error::InvalidHeader)?;
        let atlas_offset = header_size
            .checked_add(glyph_bytes)
            .ok_or(Error::InvalidHeader)?;
        let end = atlas_offset
            .checked_add(atlas_bytes)
            .ok_or(Error::InvalidHeader)?;
        if bytes.len() < end {
            return Err(Error::TooShort);
        }
        validate_glyph_table(
            bytes,
            header_size,
            glyph_count,
            cell_width,
            cell_height,
            atlas_width as usize,
            atlas_height as usize,
        )?;

        Ok(Self {
            bytes,
            header_size,
            glyph_count,
            atlas_offset,
            atlas_bytes,
            cell_width,
            cell_height,
            cell_advance,
            line_height,
            atlas_width,
            atlas_height,
            fallback_index,
        })
    }

    pub const fn cell_width(self) -> u32 {
        self.cell_width
    }

    pub const fn cell_height(self) -> u32 {
        self.cell_height
    }

    pub const fn cell_advance(self) -> u32 {
        self.cell_advance
    }

    pub const fn line_height(self) -> u32 {
        self.line_height
    }

    pub const fn atlas_width(self) -> u32 {
        self.atlas_width
    }

    pub const fn atlas_height(self) -> u32 {
        self.atlas_height
    }

    pub fn atlas_alpha(self) -> &'a [u8] {
        &self.bytes[self.atlas_offset..self.atlas_offset + self.atlas_bytes]
    }

    pub fn glyph_or_fallback(self, codepoint: u32) -> Result<Glyph, Error> {
        if let Some(glyph) = self.glyph(codepoint)? {
            return Ok(glyph);
        }
        self.glyph_at(self.fallback_index)
    }

    pub fn glyph(self, codepoint: u32) -> Result<Option<Glyph>, Error> {
        let mut low = 0usize;
        let mut high = self.glyph_count;
        while low < high {
            let mid = low + (high - low) / 2;
            let glyph = self.glyph_at(mid)?;
            if glyph.codepoint == codepoint {
                return Ok(Some(glyph));
            }
            if glyph.codepoint < codepoint {
                low = mid + 1;
            } else {
                high = mid;
            }
        }
        Ok(None)
    }

    fn glyph_at(self, index: usize) -> Result<Glyph, Error> {
        if index >= self.glyph_count {
            return Err(Error::InvalidGlyph);
        }
        let offset = self.header_size + index * GLYPH_RECORD_BYTES;
        Ok(Glyph {
            codepoint: read_u32(self.bytes, offset)?,
            advance: read_u16(self.bytes, offset + 4)?,
            bearing_x: read_i16(self.bytes, offset + 6)?,
            top_offset: read_i16(self.bytes, offset + 8)?,
            width: read_u16(self.bytes, offset + 10)?,
            height: read_u16(self.bytes, offset + 12)?,
            atlas_x: read_u16(self.bytes, offset + 14)?,
            atlas_y: read_u16(self.bytes, offset + 16)?,
        })
    }
}

fn validate_glyph_table(
    bytes: &[u8],
    header_size: usize,
    glyph_count: usize,
    cell_width: u32,
    cell_height: u32,
    atlas_width: usize,
    atlas_height: usize,
) -> Result<(), Error> {
    let mut previous_codepoint = INITIAL_PREVIOUS_CODEPOINT;
    for index in 0..glyph_count {
        let glyph = read_glyph_record(bytes, header_size + index * GLYPH_RECORD_BYTES)?;
        if index != 0 && glyph.codepoint <= previous_codepoint {
            return Err(Error::InvalidGlyph);
        }
        validate_glyph_cell(glyph, cell_width, cell_height, atlas_width, atlas_height)?;
        previous_codepoint = glyph.codepoint;
    }
    Ok(())
}

fn validate_glyph_cell(
    glyph: Glyph,
    cell_width: u32,
    cell_height: u32,
    atlas_width: usize,
    atlas_height: usize,
) -> Result<(), Error> {
    if glyph.width as u32 > cell_width || glyph.height as u32 > cell_height {
        return Err(Error::InvalidGlyph);
    }

    let cell_right = (glyph.atlas_x as usize)
        .checked_add(cell_width as usize)
        .ok_or(Error::InvalidGlyph)?;
    let cell_bottom = (glyph.atlas_y as usize)
        .checked_add(cell_height as usize)
        .ok_or(Error::InvalidGlyph)?;
    if cell_right > atlas_width || cell_bottom > atlas_height {
        return Err(Error::InvalidGlyph);
    }
    Ok(())
}

fn read_u16(bytes: &[u8], offset: usize) -> Result<u16, Error> {
    let raw = bytes.get(offset..offset + 2).ok_or(Error::TooShort)?;
    Ok(u16::from_le_bytes([raw[0], raw[1]]))
}

fn read_i16(bytes: &[u8], offset: usize) -> Result<i16, Error> {
    let raw = read_u16(bytes, offset)?;
    Ok(i16::from_le_bytes(raw.to_le_bytes()))
}

fn read_u32(bytes: &[u8], offset: usize) -> Result<u32, Error> {
    let raw = bytes.get(offset..offset + 4).ok_or(Error::TooShort)?;
    Ok(u32::from_le_bytes([raw[0], raw[1], raw[2], raw[3]]))
}

fn write_u32(bytes: &mut [u8], offset: usize, value: u32) -> Result<(), Error> {
    let raw = bytes.get_mut(offset..offset + 4).ok_or(Error::TooShort)?;
    raw.copy_from_slice(&value.to_le_bytes());
    Ok(())
}
