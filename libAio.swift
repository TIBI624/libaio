import Foundation

// C API declarations (automatically linked if you provide a bridging header or modulemap)
func libaio_c_init(_ capacity: UInt) -> Int32
func libaio_c_submit(_ miniapp: Int32, _ type: Int32, _ fd: Int32, _ buf: UnsafeMutableRawPointer, _ len: UInt, _ off: UInt64, _ cb: UnsafeMutableRawPointer?, _ ctx: UnsafeMutableRawPointer?) -> Int32
func libaio_c_poll(_ timeout: UInt32) -> UInt64
func libaio_c_destroy()

public class LibAioManager {
    public static let shared = LibAioManager()
    
    private init() {}
    
    public func initialize(capacity: UInt = 1024) -> Bool {
        return libaio_c_init(capacity) == 0
    }
    
    public func submitRead(miniapp: Int32, fd: Int32, buffer: UnsafeMutableRawPointer, length: UInt, offset: UInt64, context: UnsafeMutableRawPointer?) -> Bool {
        return libaio_c_submit(miniapp, 0, fd, buffer, length, offset, nil, context) == 0
    }
    
    public func submitWrite(miniapp: Int32, fd: Int32, buffer: UnsafeMutableRawPointer, length: UInt, offset: UInt64, context: UnsafeMutableRawPointer?) -> Bool {
        return libaio_c_submit(miniapp, 1, fd, buffer, length, offset, nil, context) == 0
    }
    
    public func poll(timeoutMs: UInt32 = 0) -> UInt64 {
        return libaio_c_poll(timeoutMs)
    }
    
    deinit {
        libaio_c_destroy()
    }
}