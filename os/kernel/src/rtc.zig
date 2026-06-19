const cmos_index_port: u16 = 0x70;
const cmos_data_port: u16 = 0x71;
const nmi_disable: u8 = 0x80;

fn outb(port: u16, value: u8) void {
    asm volatile ("outb %[value], %[port]"
        :
        : [value] "{al}" (value),
          [port] "{dx}" (port),
    );
}

fn inb(port: u16) u8 {
    return asm volatile ("inb %[port], %[value]"
        : [value] "={al}" (-> u8),
        : [port] "{dx}" (port),
    );
}

fn readRegister(reg: u8) u8 {
    outb(cmos_index_port, nmi_disable | reg);
    return inb(cmos_data_port);
}

fn updateInProgress() bool {
    return (readRegister(0x0A) & 0x80) != 0;
}

fn bcdToBinary(value: u8) u8 {
    return (value & 0x0F) + ((value >> 4) * 10);
}

const RtcSample = struct {
    second: u8,
    minute: u8,
    hour: u8,
    day: u8,
    month: u8,
    year: u8,
    century: u8,
    status_b: u8,
};

fn readSample() ?RtcSample {
    var spin: usize = 0;
    while (updateInProgress()) : (spin += 1) {
        if (spin > 1_000_000) return null;
    }
    return .{
        .second = readRegister(0x00),
        .minute = readRegister(0x02),
        .hour = readRegister(0x04),
        .day = readRegister(0x07),
        .month = readRegister(0x08),
        .year = readRegister(0x09),
        .century = readRegister(0x32),
        .status_b = readRegister(0x0B),
    };
}

fn sameTime(a: RtcSample, b: RtcSample) bool {
    return a.second == b.second and
        a.minute == b.minute and
        a.hour == b.hour and
        a.day == b.day and
        a.month == b.month and
        a.year == b.year and
        a.century == b.century and
        a.status_b == b.status_b;
}

fn stableSample() ?RtcSample {
    var attempt: usize = 0;
    while (attempt < 8) : (attempt += 1) {
        const a = readSample() orelse return null;
        const b = readSample() orelse return null;
        if (sameTime(a, b)) return a;
    }
    return null;
}

fn isLeapYear(year: u32) bool {
    return (year % 4 == 0 and year % 100 != 0) or (year % 400 == 0);
}

fn daysBeforeMonth(year: u32, month: u8) u32 {
    const common = [_]u16{ 0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334 };
    var days: u32 = common[month - 1];
    if (month > 2 and isLeapYear(year)) days += 1;
    return days;
}

fn daysBeforeYear(year: u32) u64 {
    const y = @as(u64, year - 1970);
    return y * 365 + ((@as(u64, year - 1) / 4) - 1969 / 4) - ((@as(u64, year - 1) / 100) - 1969 / 100) + ((@as(u64, year - 1) / 400) - 1969 / 400);
}

fn epochSeconds(year: u32, month: u8, day: u8, hour: u8, minute: u8, second: u8) ?u64 {
    if (year < 1970 or month < 1 or month > 12 or day < 1 or day > 31 or hour > 23 or minute > 59 or second > 60) return null;
    const days = daysBeforeYear(year) + daysBeforeMonth(year, month) + @as(u64, day - 1);
    return days * 86_400 + @as(u64, hour) * 3600 + @as(u64, minute) * 60 + @as(u64, second);
}

pub fn unixTimeSeconds() u64 {
    var s = stableSample() orelse return 0;
    const binary_mode = (s.status_b & 0x04) != 0;
    const hour_pm = (s.hour & 0x80) != 0;

    if (!binary_mode) {
        s.second = bcdToBinary(s.second);
        s.minute = bcdToBinary(s.minute);
        s.hour = bcdToBinary(s.hour & 0x7F);
        s.day = bcdToBinary(s.day);
        s.month = bcdToBinary(s.month);
        s.year = bcdToBinary(s.year);
        s.century = bcdToBinary(s.century);
    } else {
        s.hour &= 0x7F;
    }

    if ((s.status_b & 0x02) == 0) {
        if (hour_pm and s.hour < 12) s.hour += 12;
        if (!hour_pm and s.hour == 12) s.hour = 0;
    }

    var full_year: u32 = undefined;
    if (s.century >= 19 and s.century <= 99) {
        full_year = @as(u32, s.century) * 100 + s.year;
    } else {
        full_year = if (s.year < 70) 2000 + @as(u32, s.year) else 1900 + @as(u32, s.year);
    }

    return epochSeconds(full_year, s.month, s.day, s.hour, s.minute, s.second) orelse 0;
}
