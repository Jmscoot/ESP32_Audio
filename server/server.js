const express = require('express');        // express lib for server
const fs = require('fs');                  // fs for file RW
const readline = require('readline');      // keyboard in/out lib
const path = require('path');              // path manipulation


const fileListApp = express();             
const audioApp = express();                


let currentFile = '';                      // current file name save var


const rl = readline.createInterface({
    input: process.stdin,                  
    output: process.stdout                
});



const fileListPath = path.join(__dirname, 'uploads', 'filelist.txt');



function cleanupFiles() {
    console.log('\nprogram logout...');
    

    if (fs.existsSync(fileListPath)) {
        try {
            fs.unlinkSync(fileListPath);
            console.log('filelist.txt is deleted');
        } catch (error) {
            console.error('filelist.txt delete error:', error.message);
        }
    }
    
    console.log('logout complete.');
    process.exit(0);
}



process.on('SIGINT', cleanupFiles);     
process.on('SIGTERM', cleanupFiles);   
process.on('SIGQUIT', cleanupFiles);    
process.on('exit', () => {              
    console.log('process quit.');
});


process.on('uncaughtException', (error) => {
    console.error('error:', error);
    cleanupFiles();
});


process.on('unhandledRejection', (reason, promise) => {
    console.error('처리되지 않은 Promise 거부:', reason);
    cleanupFiles();
});


//port 3001 filelist server
fileListApp.get('/filelist.txt', (req, res) => {
    console.log('[port3001] eps32 req filelist.txt');    
    res.send(currentFile);                               
});


//port 3000 .raw req
audioApp.get('/*path', (req, res) => {
    const fileName = req.url.slice(1);        
    console.log('[port 3000] ESP32 req file:', fileName);
    
    if (fs.existsSync(fileName)) {
        console.log('[port 3000] file send start:', fileName);
        res.sendFile(__dirname + '/' + fileName);  
    } else {
        console.log('[port 3000] file doesn\'t exist:', fileName);
        res.status(404).send('the file doesn\'t exist');         
    }
});


// start server func
function startServer() {
    // port 3001: filelist.txt server start
    fileListApp.listen(3001, () => {
        console.log('filelist server start - port 3001');
        console.log('   ESP32 polling addr: http://localhost:3501/filelist.txt');
    });
    
    // port 3000: .raw server start
    audioApp.listen(3000, () => {
        console.log('.raw server start - port 3000');
        console.log('   ESP32 download addr: http://localhost:3500/' + currentFile);
        console.log('current file:', currentFile);
        
        // if upload folder doesn't exist, make it!
        if (!fs.existsSync('uploads')) {
            fs.mkdirSync('uploads');
            console.log('uploads folder is made');
        }
        
        // making filelist.txt 
        fs.writeFileSync(fileListPath, currentFile);
        console.log('filelist.txt is made');
        console.log('\nctrl+c for exit');
    });
}


// program start here
rl.question('enter file .raw file: ', (answer) => {      
    if (!answer.trim()) {
        console.log('empty file dir');
        rl.close();                            
        return;
    }
    
    currentFile = answer;                      
    console.log('selected file:', currentFile);
    
    if (fs.existsSync(currentFile)) {
        console.log('file detected!');
        rl.close();                             
        startServer();                          
    } else {
        console.log('file doesn\'t exist:', currentFile);
        console.log('insert the file in the dir and restart');
        rl.close();                              
    }
});
