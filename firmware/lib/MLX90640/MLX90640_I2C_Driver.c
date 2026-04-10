/**
 * MLX90640_I2C_Driver.c — ESP-IDF I2C implementation for MLX90640
 *
 * Implements the Melexis I2C driver API using ESP-IDF's i2c driver.
 * The MLX90640 uses 16-bit register addresses and 16-bit data words
 * (big-endian on the wire).
 */

#include "MLX90640_I2C_Driver.h"
#include "driver/i2c.h"
#include "esp_log.h"

static const char *TAG = "mlx_i2c";

#define I2C_PORT        I2C_NUM_0
#define I2C_TIMEOUT_MS  1000

int MLX90640_I2CRead(uint8_t slaveAddr, uint16_t startAddr,
                     uint16_t nMemAddressRead, uint16_t *data)
{
    if (nMemAddressRead == 0 || data == NULL) {
        return -1;
    }

    /* Build the address bytes (big-endian 16-bit register address) */
    uint8_t addr_buf[2] = {
        (uint8_t)(startAddr >> 8),
        (uint8_t)(startAddr & 0xFF)
    };

    /* Allocate buffer for raw bytes (2 bytes per word) */
    uint16_t byte_count = nMemAddressRead * 2;
    uint8_t *raw = (uint8_t *)malloc(byte_count);
    if (!raw) {
        ESP_LOGE(TAG, "malloc failed for %u bytes", byte_count);
        return -1;
    }

    /* I2C write-then-read transaction:
     * START → [addr+W] → [reg_hi] → [reg_lo] → RESTART → [addr+R] → [data...] → STOP
     */
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (slaveAddr << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write(cmd, addr_buf, 2, true);
    i2c_master_start(cmd);  /* Repeated start */
    i2c_master_write_byte(cmd, (slaveAddr << 1) | I2C_MASTER_READ, true);
    if (byte_count > 1) {
        i2c_master_read(cmd, raw, byte_count - 1, I2C_MASTER_ACK);
    }
    i2c_master_read_byte(cmd, &raw[byte_count - 1], I2C_MASTER_NACK);
    i2c_master_stop(cmd);

    esp_err_t err = i2c_master_cmd_begin(I2C_PORT, cmd, pdMS_TO_TICKS(I2C_TIMEOUT_MS));
    i2c_cmd_link_delete(cmd);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I2C read failed at 0x%04X (%u words): %s",
                 startAddr, nMemAddressRead, esp_err_to_name(err));
        free(raw);
        return -1;
    }

    /* Convert big-endian byte pairs to uint16_t */
    for (uint16_t i = 0; i < nMemAddressRead; i++) {
        data[i] = ((uint16_t)raw[2 * i] << 8) | raw[2 * i + 1];
    }

    free(raw);
    return 0;
}

int MLX90640_I2CWrite(uint8_t slaveAddr, uint16_t writeAddr, uint16_t data)
{
    /* Pack: [reg_addr_hi] [reg_addr_lo] [data_hi] [data_lo] */
    uint8_t buf[4] = {
        (uint8_t)(writeAddr >> 8),
        (uint8_t)(writeAddr & 0xFF),
        (uint8_t)(data >> 8),
        (uint8_t)(data & 0xFF)
    };

    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (slaveAddr << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write(cmd, buf, 4, true);
    i2c_master_stop(cmd);

    esp_err_t err = i2c_master_cmd_begin(I2C_PORT, cmd, pdMS_TO_TICKS(I2C_TIMEOUT_MS));
    i2c_cmd_link_delete(cmd);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I2C write failed at 0x%04X: %s", writeAddr, esp_err_to_name(err));
        return -1;
    }

    return 0;
}

int MLX90640_I2CGeneralReset(void)
{
    /* I2C general call reset: address 0x00, command 0x06 */
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, 0x00, true);  /* General call address */
    i2c_master_write_byte(cmd, 0x06, true);  /* Reset command */
    i2c_master_stop(cmd);
    esp_err_t err = i2c_master_cmd_begin(I2C_PORT, cmd, pdMS_TO_TICKS(I2C_TIMEOUT_MS));
    i2c_cmd_link_delete(cmd);
    return (err == ESP_OK) ? 0 : -1;
}

void MLX90640_I2CFreqSet(int freq)
{
    /* Reconfigure I2C clock speed */
    i2c_config_t conf = {0};
    i2c_param_config(I2C_PORT, &conf);
    /* Note: ESP-IDF doesn't support changing frequency without reinit.
     * For now, this is a no-op — we initialize at 400kHz and stay there. */
    ESP_LOGI(TAG, "I2C frequency set request: %dkHz (using fixed 400kHz)", freq);
}
