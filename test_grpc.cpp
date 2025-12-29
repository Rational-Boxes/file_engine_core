#include <iostream>
#include <memory>
#include <string>

#include "core/include/fileengine/grpc_service.h"
#include "proto/fileengine.grpc.pb.h"
#include <grpcpp/grpcpp.h>

int main() {
    std::cout << "Testing gRPC connection..." << std::endl;
    
    try {
        // Connect to the server
        std::string server_address = "localhost:50051";
        auto channel = grpc::CreateChannel(server_address, grpc::InsecureChannelCredentials());
        
        if (!channel) {
            std::cout << "Failed to create channel" << std::endl;
            return 1;
        }
        
        // Test if the channel is connected
        std::cout << "Channel state: " << channel->GetState(true) << std::endl;
        
        // Create stub
        auto stub = fileengine::FileEngineService::NewStub(channel);
        
        if (!stub) {
            std::cout << "Failed to create stub" << std::endl;
            return 1;
        }
        
        std::cout << "Successfully created gRPC stub!" << std::endl;
        
        // Try a simple operation - create a directory request
        fileengine::MkdirRequest request;
        request.set_parent_uid("");
        request.set_name("test_dir");
        request.set_user("root");
        request.set_permissions(0755);
        
        fileengine::MkdirResponse response;
        grpc::ClientContext context;
        
        std::cout << "Attempting mkdir operation..." << std::endl;
        grpc::Status status = stub->Mkdir(&context, request, &response);
        
        if (status.ok()) {
            std::cout << "Mkdir operation successful! UID: " << response.uid() << std::endl;
        } else {
            std::cout << "Mkdir operation failed: " << status.error_code() << ": " << status.error_message() << std::endl;
        }
        
    } catch (const std::exception& e) {
        std::cout << "Exception occurred: " << e.what() << std::endl;
        return 1;
    }
    
    std::cout << "gRPC test completed." << std::endl;
    return 0;
}