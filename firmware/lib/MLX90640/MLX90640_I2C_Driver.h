/**
 * MLX90640_I2C_Driver.h — I2C driver interface for MLX90640
 *
 * Platform-specific I2C implementation for ESP-IDF.
 * Based on the Melexis reference driver API.
 */

#ifndef MLX90640_I2C_DRIVER_H
#define MLX90640_I2C_DRIVER_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Read a block of 16-bit words from the MLX90640 over I2C.
 *
 * @param slaveAddr  I2C slave address (0x33 default)
 * @param startAddr  Start register address (16-bit)
 * @param nMemAddressRead  Number of 16-bit words to read
 * @param data  Output buffer for the read data
 * @return 0 on success, -1 on failure
 */
int MLX90640_I2CRead(uint8_t slaveAddr, uint16_t startAddr,
                     uint16_t nMemAddressRead, uint16_t *data);

/**
 * Write a single 16-bit word to the MLX90640 over I2C.
 *
 * @param slaveAddr  I2C slave address
 * @param writeAddr  Register address to write
 * @param data  16-bit value to write
 * @return 0 on success, -1 on failure
 */
int MLX90640_I2CWrite(uint8_t slaveAddr, uint16_t writeAddr, uint16_t data);

/**
 * Set I2C bus frequency.
 *
 * @param slaveAddr  I2C slave address (unused, for API compat)
 * @param freq  Frequency in kHz (100, 400, 1000)
 */
void MLX90640_I2CFreqSet(int freq);

/**
 * Send I2C general call reset (address 0x00, command 0x06).
 * Resets all devices on the bus. Used by MLX90640 trigger measurement.
 */
int MLX90640_I2CGeneralReset(void);

#ifdef __cplusplus
}
#endif

#endif /* MLX90640_I2C_DRIVER_H */
