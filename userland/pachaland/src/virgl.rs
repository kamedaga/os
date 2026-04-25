pub const BIND_OBJECT_WORDS: u32 = 1;
pub const SUPPORTED_COMMANDS: [Command; 4] = [
    Command::BindObject,
    Command::SetFramebufferState,
    Command::Clear,
    Command::DrawVbo,
];
pub const SUPPORTED_OBJECT_KINDS: [ObjectKind; 9] = [
    ObjectKind::Null,
    ObjectKind::Blend,
    ObjectKind::Dsa,
    ObjectKind::Rasterizer,
    ObjectKind::Shader,
    ObjectKind::SamplerView,
    ObjectKind::SamplerState,
    ObjectKind::VertexElements,
    ObjectKind::Surface,
];

#[derive(Copy, Clone, Debug, Eq, PartialEq)]
#[repr(u32)]
pub enum Command {
    BindObject = 2,
    SetFramebufferState = 12,
    Clear = 20,
    DrawVbo = 43,
}

#[derive(Copy, Clone, Debug, Eq, PartialEq)]
#[repr(u32)]
pub enum ObjectKind {
    Null = 0,
    Blend = 1,
    Dsa = 2,
    Rasterizer = 3,
    Shader = 4,
    SamplerView = 5,
    SamplerState = 6,
    VertexElements = 7,
    Surface = 8,
}

pub const fn command_header(command: Command, object: ObjectKind, len_words: u32) -> u32 {
    (command as u32) | ((object as u32) << 8) | (len_words << 16)
}

#[derive(Copy, Clone, Debug, Eq, PartialEq)]
#[allow(dead_code)]
pub struct ImportedSurface {
    pub surface_id: u32,
    pub resource_id: u32,
    pub width: u32,
    pub height: u32,
    pub generation: u32,
}

impl ImportedSurface {
    #[allow(dead_code)]
    pub const fn valid(self) -> bool {
        self.surface_id != 0 && self.resource_id != 0 && self.width != 0 && self.height != 0
    }
}
