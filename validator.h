#ifndef VALIDATOR_H
#define VALIDATOR_H

#include <string>
#include <vector>

// Success validation for different protocols
// Validates user-level success vs network-level success

class SuccessValidator {
public:
    SuccessValidator();
    
    // Validate HTTP/HTTPS response
    // Returns (network_success, user_success)
    std::pair<bool, bool> validate_http(uint16_t status_code, const std::vector<uint8_t>& body);
    
private:
    bool contains_error_patterns(const std::string& content) const;
};

#endif // VALIDATOR_H
