#include <dev/device.h>
#include <dev/driver.h>
#include <lock.h>
#include <ck/vec.h>

static spinlock all_devices_lock;
static ck::vec<ck::ref<dev::Device>> all_devices;

dev::Device::Device(DeviceType t) : m_type(t) {}


// Register a device with the global device system
void dev::Device::add(ck::string name, ck::ref<Device> dev) {
  dev->set_name(name);

  KINFO("Add Device %s\n", name.get());

  scoped_lock l(all_devices_lock);
  assert(all_devices.find(dev).is_end());
  all_devices.push(dev);

	dev::Driver::probe_all(dev);
  // TODO: probe all drivers
}


// Register a device with the global device system
void dev::Device::remove(ck::ref<Device> dev, RemovalReason reason) {
  scoped_lock l(all_devices_lock);
  // TODO: Notify drivers that the device has been removed, and why
  all_devices.remove_first_matching([dev](auto other) {
    return other == dev;
  });
}

ck::vec<ck::ref<dev::Device>> dev::Device::all(void) {
  scoped_lock l(all_devices_lock);
  ck::vec<ck::ref<dev::Device>> all = all_devices;

  return all;
}



bool dev::PCIDevice::is_device(uint16_t vendor, uint16_t device) const { return vendor_id == vendor && device_id == device; }