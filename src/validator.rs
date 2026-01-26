pub struct SuccessValidator;

impl SuccessValidator {
    pub fn new() -> Self {
        Self
    }

    pub async fn validate_http(&self, status: u16, body: &[u8]) -> (bool, bool) {
        let network_success = status >= 200 && status < 400;
        
        if !network_success {
            return (false, false);
        }

        let user_success = if !body.is_empty() {
            let content_str = String::from_utf8_lossy(body).to_lowercase();
            let error_patterns = ["blocked", "forbidden", "access denied", "error 403", "error 404"];
            !error_patterns.iter().any(|pattern| content_str.contains(pattern))
        } else {
            false
        };

        (network_success, user_success)
    }
}
